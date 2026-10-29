#include "helper_loader.hpp"
#include "helper_payload.h"
#include "version_store.hpp"
#include "checksum.hpp"
#include "crypto.hpp"
#include <atomic>
#include <dlfcn.h>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace pmu {
namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
struct HelperLoader::Impl {
    VersionStore store;
    std::string helper,architecture,payload,bootstrap,id;
    void* module=nullptr;
    PmuHelperPayloadV1 api{};
    PmuHelperHostV1 host{};
    std::atomic<bool> accepting{false},observed{false},claimed{false},ready{false};
    mutable std::mutex errorMutex;
    std::string lastError;
    Impl(std::filesystem::path location,std::filesystem::path root,std::string arch,std::string publisher):store(location,geteuid(),[](const auto& path){return fileSha256(path);},{},false,[publisher](const auto& id,const auto&,const auto& proof){verifyAuthorization(publisher,"helper",id,proof);}),helper(root.string()),architecture(std::move(arch)){
        need(root.is_absolute()&&root.lexically_normal()==root&&root!=root.root_path()&&location.parent_path()==root,"Helper version store is outside its installation");need(architecture=="armv7"||architecture=="aarch64","Unsupported helper architecture");
    }
    void failure(const std::string& text){std::lock_guard<std::mutex> guard(errorMutex);lastError=text;}
    void confirmOnce(){
        if(!accepting.load()||!observed.load())return;
        bool expected=false;if(!claimed.compare_exchange_strong(expected,true))return;
        try{store.confirm(id);ready.store(true);}catch(const std::exception& error){failure(error.what());}
    }
    static void confirmed(void* data){auto& self=*static_cast<Impl*>(data);self.observed.store(true);self.confirmOnce();}
    void stop(){
        accepting.store(false);
        if(api.stop)try{api.stop();}catch(...){failure("Helper payload failed to stop");}
        api={};if(module){dlclose(module);module=nullptr;}ready.store(false);
    }
    bool load(const Generation& generation,void* binding){
        need(generation.spec.architecture==architecture||generation.spec.architecture=="universal","Helper generation has the wrong architecture");const auto role=generation.spec.entrypoints.find("helper_"+architecture),script=generation.spec.entrypoints.find("bootstrap");need(role!=generation.spec.entrypoints.end()&&script!=generation.spec.entrypoints.end(),"Helper generation lacks its startup roles");
        id=generation.id;payload=generation.directory.string();bootstrap=(generation.directory/script->second).string();
        const auto path=store.entrypoint(id,"helper_"+architecture);module=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);need(module!=nullptr,"Cannot load the verified helper payload");
        auto query=reinterpret_cast<PmuHelperPayloadQueryV1>(dlsym(module,"pmu_helper_payload_v1"));need(query!=nullptr,"Helper payload has no stable startup interface");const auto* offered=query();need(offered&&offered->abi_version==1&&offered->struct_size>=sizeof(PmuHelperPayloadV1)&&offered->start&&offered->stop,"Helper payload startup interface is incompatible");api=*offered;
        host={1,sizeof(PmuHelperHostV1),helper.c_str(),payload.c_str(),bootstrap.c_str(),id.c_str(),architecture.c_str(),this,confirmed};observed.store(false);claimed.store(false);
        if(api.start(&host,binding)!=1)return false;
        accepting.store(true);confirmOnce();return true;
    }
};
HelperLoader::HelperLoader(std::filesystem::path store,std::filesystem::path helper,std::string architecture,std::string publisher):impl(std::make_unique<Impl>(std::move(store),std::move(helper),std::move(architecture),std::move(publisher))){}
HelperLoader::~HelperLoader(){impl->stop();}
bool HelperLoader::start(void* binding){
    if(impl->module)return true;
    std::set<std::string> attempted;
    for(int attempt=0;attempt<2;++attempt){
        try{const auto selected=impl->store.boot();need(selected.has_value(),"Helper version store is not initialized");need(attempted.insert(selected->id).second,"No working helper payload is available");if(impl->load(*selected,binding))return true;impl->failure("Helper payload refused startup");}
        catch(const std::exception& error){impl->failure(error.what());}
        impl->stop();
    }
    return false;
}
void HelperLoader::stop(){impl->stop();}
bool HelperLoader::healthy()const{return impl->ready.load();}
std::string HelperLoader::generation()const{return impl->id;}
std::string HelperLoader::error()const{std::lock_guard<std::mutex> guard(impl->errorMutex);return impl->lastError;}
}
