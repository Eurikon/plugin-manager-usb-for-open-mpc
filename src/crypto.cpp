#include "crypto.hpp"
#include "metadata.hpp"
#include <array>
#include <dlfcn.h>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <vector>

namespace pmu {
namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Close {void operator()(void* value)const{if(value)dlclose(value);}};
using Library=std::unique_ptr<void,Close>;
Library crypto(){Library value(dlopen("libcrypto.so.3",RTLD_NOW|RTLD_LOCAL));if(!value)value.reset(dlopen("libcrypto.so.1.1",RTLD_NOW|RTLD_LOCAL));need(bool(value),"Signature-verification library is unavailable");return value;}
template<class Function> Function symbol(void* library,const char* name){auto result=reinterpret_cast<Function>(dlsym(library,name));need(result!=nullptr,"Firmware signature-verification API is unavailable");return result;}
std::vector<unsigned char> unhex(const std::string& text,size_t size){
    need(text.size()==size*2&&text.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid publisher key or signature encoding");
    auto digit=[](char value){return value<='9'?value-'0':value-'a'+10;};std::vector<unsigned char> result(size);for(size_t i=0;i<size;++i)result[i]=static_cast<unsigned char>(digit(text[i*2])*16+digit(text[i*2+1]));return result;
}
}
std::string sha256Text(std::string_view text){
    need(text.size()<=1024*1024,"Oversized metadata digest input");auto library=crypto();const auto digest=symbol<decltype(&EVP_Digest)>(library.get(),"EVP_Digest");const auto algorithm=symbol<decltype(&EVP_sha256)>(library.get(),"EVP_sha256");std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{};unsigned length=0;need(digest(text.data(),text.size(),bytes.data(),&length,algorithm(),nullptr)==1&&length==32,"Cannot hash signed metadata");
    std::string result;const char* digits="0123456789abcdef";for(size_t i=0;i<length;++i){result+=digits[bytes[i]>>4];result+=digits[bytes[i]&15];}return result;
}
bool verifyEd25519(const std::string& publicKey,std::string_view message,const std::string& signature){
    need(message.size()<=128*1024,"Oversized signed metadata");const auto key=unhex(publicKey,32),signedBytes=unhex(signature,64);auto library=crypto();
    auto newKey=symbol<decltype(&EVP_PKEY_new_raw_public_key)>(library.get(),"EVP_PKEY_new_raw_public_key");auto freeKey=symbol<decltype(&EVP_PKEY_free)>(library.get(),"EVP_PKEY_free");
    auto newContext=symbol<decltype(&EVP_MD_CTX_new)>(library.get(),"EVP_MD_CTX_new");auto freeContext=symbol<decltype(&EVP_MD_CTX_free)>(library.get(),"EVP_MD_CTX_free");
    auto initialize=symbol<decltype(&EVP_DigestVerifyInit)>(library.get(),"EVP_DigestVerifyInit");auto verify=symbol<decltype(&EVP_DigestVerify)>(library.get(),"EVP_DigestVerify");
    std::unique_ptr<EVP_PKEY,decltype(freeKey)> pkey(newKey(EVP_PKEY_ED25519,nullptr,key.data(),key.size()),freeKey);std::unique_ptr<EVP_MD_CTX,decltype(freeContext)> context(newContext(),freeContext);need(bool(pkey)&&bool(context)&&initialize(context.get(),nullptr,nullptr,nullptr,pkey.get())==1,"Cannot initialize publisher signature verification");return verify(context.get(),signedBytes.data(),signedBytes.size(),reinterpret_cast<const unsigned char*>(message.data()),message.size())==1;
}
void verifyAuthorization(const std::string& publicKey,const std::string& kind,const std::string& generation,const std::string& proof){
    need((kind=="runtime"||kind=="helper"||kind=="update")&&generation.size()==64&&generation.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid requested publisher authorization");need(!proof.empty()&&proof.size()<=8192,"Missing or oversized publisher authorization");const auto record=parseMetadata(proof);need(record.is_object()&&record.size()==2,"Invalid publisher authorization record");const auto message=record.at("message").get<std::string>();need(message.size()<=2048,"Oversized publisher authorization message");const auto body=parseMetadata(message);
    need(body.is_object()&&body.size()==4&&body.value("schema",0)==1&&body.value("product",std::string())=="plugin-manager-usb"&&body.value("kind",std::string())==kind&&body.value("generation",std::string())==generation&&body.dump()+'\n'==message,"Publisher authorization does not match this payload");need(verifyEd25519(publicKey,message,record.at("signature").get<std::string>()),"Publisher signature verification failed");
}
}
