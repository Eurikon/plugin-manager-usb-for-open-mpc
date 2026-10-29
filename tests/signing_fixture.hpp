#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <string>

namespace pmu::test {
class SigningFixture {
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key{nullptr,EVP_PKEY_free};
    static std::string hex(const unsigned char* bytes,size_t size){std::string result;const char* digits="0123456789abcdef";for(size_t i=0;i<size;++i){result+=digits[bytes[i]>>4];result+=digits[bytes[i]&15];}return result;}
public:
    SigningFixture(){std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,nullptr),EVP_PKEY_CTX_free);EVP_PKEY* generated=nullptr;if(!context||EVP_PKEY_keygen_init(context.get())!=1||EVP_PKEY_keygen(context.get(),&generated)!=1)throw std::runtime_error("Cannot generate ephemeral test key");key.reset(generated);}
    std::string publicKey()const{std::array<unsigned char,32> bytes{};size_t size=bytes.size();if(EVP_PKEY_get_raw_public_key(key.get(),bytes.data(),&size)!=1||size!=bytes.size())throw std::runtime_error("Cannot extract test public key");return hex(bytes.data(),size);}
    std::string signature(const std::string& message)const{std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);std::array<unsigned char,64> bytes{};size_t size=bytes.size();if(!context||EVP_DigestSignInit(context.get(),nullptr,nullptr,nullptr,key.get())!=1||EVP_DigestSign(context.get(),bytes.data(),&size,reinterpret_cast<const unsigned char*>(message.data()),message.size())!=1||size!=bytes.size())throw std::runtime_error("Cannot sign test metadata");return hex(bytes.data(),size);}
    std::string proof(const std::string& id,const std::string& kind)const{const std::string message=nlohmann::json{{"schema",1},{"product","plugin-manager-usb"},{"generation",id},{"kind",kind}}.dump()+'\n';return nlohmann::json{{"message",message},{"signature",signature(message)}}.dump()+'\n';}
};
}
