#include "source_runtime.hpp"
#include "update_bundle.hpp"
#include "migration_package.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "helper_payload.h"
#include "platform.hpp"
#include <publisher_key.hpp>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="--pmu-update-v1")return pmu::serveUpdateWorker([](const auto& task,const auto&){
        pmu::SourceRuntime source(task.at("source").template get<std::string>(),managerArchitecture(),pmu::publisherKey);source.choose(false);
        const int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("Probe cannot open its running image");
        const auto name="/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);char target[4096];const auto size=readlink(name.c_str(),target,sizeof target);
        try{const auto checksum=pmu::fileSha256(name);close(fd);return nlohmann::json{{"self_sha256",checksum}};}catch(const std::exception& error){close(fd);throw std::runtime_error("Running-image hash for "+(size>0?std::string(target,size):name)+": "+error.what());}
    });
    if(argc!=3)return 2;
    try{
        const std::filesystem::path source=argv[1],helper=argv[2];if(std::filesystem::file_size(source/"PMU-UPDATE.json")>128*1024)throw std::runtime_error("Oversized factory envelope");std::ifstream stream(source/"PMU-UPDATE.json");const auto envelope=nlohmann::json::parse(stream);const auto version=envelope.at("generation").at("version").get<std::string>();const auto bundle=pmu::verifyStagedUpdate(source,managerArchitecture(),version,pmu::publisherKey);const auto deployment=pmu::readMigrationPackage(bundle,managerArchitecture());pmu::SourceRuntime runtime(source,managerArchitecture(),pmu::publisherKey);const auto selected=runtime.choose(false);if(selected.generation.id!=pmu::sha256Text(pmu::generationSpecToJson(deployment.runtime.specification).dump()+'\n'))throw std::runtime_error("Factory selection differs from the signed update payload");
        pmu::validateNativeImage(selected.worker);std::cout<<"PASS: native worker source identity\n"<<std::flush;
        char scratch[]="/tmp/pmu-stock-worker.XXXXXX";const auto made=mkdtemp(scratch);if(!made)throw std::runtime_error("Cannot create worker probe directory");{const auto pinned=pmu::pinNativeImage(selected.worker,made);if(pinned.descriptor()<0)throw std::runtime_error("Worker pinning failed");}std::cout<<"PASS: anonymous native worker pinning\n"<<std::flush;
        const auto hello=pmu::runIncomingWorker(selected.worker,made,{{"operation","hello"}});if(hello.at("protocol")!=1)throw std::runtime_error("Invalid real worker response");std::filesystem::remove_all(made);std::cout<<"PASS: actual incoming worker transport\n"<<std::flush;
        std::filesystem::create_directory(made);const pmu::NativeImageSpec probe{std::filesystem::canonical(argv[0]),pmu::fileSha256(argv[0]),managerArchitecture()};const auto own=pmu::runIncomingWorker(probe,made,{{"operation","apply"},{"source",source.string()}});if(own.at("self_sha256")!=probe.sha256)throw std::runtime_error("The running worker probe hashes another image");std::filesystem::remove_all(made);std::cout<<"PASS: running-image identification inside the incoming worker\n"<<std::flush;
        pmu::VersionStore store(helper/".pmu-versions",geteuid(),[](const auto& path){return pmu::fileSha256(path);},{},false,[](const auto& id,const auto&,const auto& proof){pmu::verifyAuthorization(pmu::publisherKey,"helper",id,proof);});const auto current=store.current();if(!current)throw std::runtime_error("Missing signed helper selection");
        const auto payload=store.entrypoint(current->id,"helper_"+std::string(managerArchitecture()));void* module=dlopen(payload.c_str(),RTLD_NOW|RTLD_LOCAL);if(!module)throw std::runtime_error(dlerror());const auto query=reinterpret_cast<PmuHelperPayloadQueryV1>(dlsym(module,"pmu_helper_payload_v1"));const auto* api=query?query():nullptr;if(!api||api->abi_version!=1||api->struct_size<sizeof(PmuHelperPayloadV1)||!api->start||!api->stop)throw std::runtime_error("Invalid native payload interface");api->stop();dlclose(module);
        const auto controller=helper/(std::string(managerArchitecture())=="aarch64"?"manager-entry-aarch64.so":"manager-entry.so");module=dlopen(controller.c_str(),RTLD_NOW|RTLD_LOCAL);if(!module)throw std::runtime_error(dlerror());if(!dlsym(module,"pmu_start_manager")||!dlsym(module,"pmu_stop_manager"))throw std::runtime_error("Missing controller exports");dlclose(module);
        std::cout<<"PASS: signed factory inventory, native source selection and helper ABI under stock libraries\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
