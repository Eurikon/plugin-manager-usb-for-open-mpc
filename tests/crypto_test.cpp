#include "crypto.hpp"
#include "signing_fixture.hpp"
#include "metadata.hpp"
#include "checksum.hpp"
#include "version_store.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& text){std::ofstream output(path);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write authorization fixture");}
using Key=pmu::test::SigningFixture;
}
int main(){
    std::string pattern="/tmp/pmu-authorization.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);const auto made=mkdtemp(name.data());if(!made)return 1;const fs::path root=made;
    try{
        check(pmu::sha256Text("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","metadata SHA-256 empty vector");check(pmu::sha256Text("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","metadata SHA-256 short vector");
        Key key,other;const auto publicKey=key.publicKey();const std::string message="release metadata";const auto signature=key.signature(message);check(pmu::verifyEd25519(publicKey,message,signature),"native library verifies an independently signed Ed25519 message");check(!pmu::verifyEd25519(publicKey,message+"changed",signature),"altered signed content is refused");check(!pmu::verifyEd25519(other.publicKey(),message,signature),"another publisher cannot authorize the payload");
        rejects([&]{pmu::verifyEd25519("bad",message,signature);},"malformed public key is refused");rejects([&]{pmu::verifyEd25519(publicKey,message,"bad");},"malformed signature is refused");
        const std::string id=pmu::sha256Text("generation");auto proof=key.proof(id,"runtime");pmu::verifyAuthorization(publicKey,"runtime",id,proof);check(true,"publisher proof binds the product, payload kind and complete generation identity");
        rejects([&]{pmu::verifyAuthorization(publicKey,"helper",id,proof);},"runtime approval cannot authorize a helper");rejects([&]{pmu::verifyAuthorization(publicKey,"runtime",std::string(64,'0'),proof);},"approval cannot authorize different code");rejects([&]{pmu::verifyAuthorization(publicKey,"runtime",id,other.proof(id,"runtime"));},"self-consistent metadata signed by another key is refused");
        rejects([&]{pmu::verifyAuthorization(publicKey,"runtime",id,"");},"missing approval is refused");rejects([&]{pmu::verifyAuthorization(publicKey,"runtime",id,std::string(8193,'x'));},"oversized approval is refused");auto wrong=Json::parse(proof);auto body=Json::parse(wrong.at("message").get<std::string>());body["product"]="another-product";wrong["message"]=body.dump()+'\n';wrong["signature"]=key.signature(wrong.at("message"));rejects([&]{pmu::verifyAuthorization(publicKey,"runtime",id,wrong.dump());},"another product's correctly signed proof is refused");
        const auto input=root/"input";fs::create_directory(input);save(input/"program","payload");auto hash=[](const auto& path){return pmu::fileSha256(path);};pmu::GenerationSpec first{"1.0.0","universal",{{"program",hash(input/"program")}},{{"manager","program"}}};const auto firstId=pmu::sha256Text(pmu::generationSpecToJson(first).dump()+'\n');
        auto trust=[&](const std::string& selected,const pmu::GenerationSpec&,const std::string& authorization){pmu::verifyAuthorization(publicKey,"runtime",selected,authorization);};pmu::VersionStore store(root/"versions",geteuid(),hash,{},true,trust);
        rejects([&]{store.stage(input,first);},"an unsigned candidate cannot enter a trusted store");const auto firstProof=key.proof(firstId,"runtime");check(store.stage(input,first,firstProof)==firstId,"signed staging identity matches the published manifest");store.seed(firstId);check(store.boot()->authorization==firstProof,"verified approval survives staging and startup");
        auto second=first;second.version="2.0.0";save(input/"program","updated payload");second.files["program"]=hash(input/"program");const auto secondId=pmu::sha256Text(pmu::generationSpecToJson(second).dump()+'\n');rejects([&]{store.stage(input,second,other.proof(secondId,"runtime"));},"a foreign signed generation is rejected before activation");check(store.current()->id==firstId,"rejected approval leaves the last working selection intact");store.stage(input,second,key.proof(secondId,"runtime"));store.activate(secondId);store.boot();store.confirm(secondId);
        save(root/"versions"/secondId/"authorization.json",other.proof(secondId,"runtime"));check(store.boot()->id==firstId,"tampered approval cannot execute a selected generation and restores the prior one");
        pmu::VersionStore external(root/"versions",geteuid(),hash);auto forged=second;forged.version="3.0.0";save(input/"program","unapproved program");forged.files["program"]=hash(input/"program");const auto forgedId=pmu::sha256Text(pmu::generationSpecToJson(forged).dump()+'\n');external.stage(input,forged,other.proof(forgedId,"runtime"));external.activate(forgedId);check(store.boot()->id==firstId,"replacing a USB manifest and all its checksums does not grant publisher approval");
        auto invalid=first;invalid.files["authorization.json"]=std::string(64,'0');rejects([&]{store.stage(input,invalid,firstProof);},"payload cannot replace its own approval record");
        fs::remove_all(root);std::cout<<"PASS: "<<checks<<" publisher-authorization binding and trusted-generation checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
