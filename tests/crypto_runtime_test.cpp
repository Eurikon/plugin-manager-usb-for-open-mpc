#include "crypto.hpp"
#include <iostream>
#include <stdexcept>

int main(){
    try{
        if(pmu::sha256Text("abc")!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")throw std::runtime_error("Native SHA-256 failed");
        const std::string key="d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
        const std::string signature="e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
        if(!pmu::verifyEd25519(key,"",signature)||pmu::verifyEd25519(key,"changed",signature))throw std::runtime_error("Native signature verification failed");
        std::cout<<"PASS: stock crypto loads dynamically and verifies hashes, signatures and altered-content refusal\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
