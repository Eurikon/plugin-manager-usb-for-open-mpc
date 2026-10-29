#include "runtime_worker.hpp"
#include "source_runtime.hpp"
#include "update_bundle.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "platform.hpp"
#include "signing_fixture.hpp"
#include "session_handoff.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);output<<value;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write runtime-worker fixture");}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Missing runtime-worker fixture");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="--pmu-update-v1")return pmu::serveUpdateWorker([](const Json& task,const auto& progress){return pmu::handleRuntimeTask(task,task.contains("publisher")?task.at("publisher").get<std::string>():task.at("test_publisher").get<std::string>(),progress,{},[](const auto& service){if(service!="acvs")throw std::runtime_error("Unexpected fixture service");});});
    char mediaPattern[]="/media/pmu-runtime-worker.XXXXXX",ramPattern[]="/tmp/pmu-runtime-worker.XXXXXX";const auto media=mkdtemp(mediaPattern),ram=mkdtemp(ramPattern);if(!media||!ram)return 1;const fs::path source=media,root=ram;
    try{
        check((fs::status(source).permissions()&fs::perms::others_write)==fs::perms::none,"worker source fixture is privately owned");
        // Emulated ARMv7 statvfs can omit mount flags; inspect the kernel record.
        bool noexec=false;std::ifstream mounts("/proc/mounts");std::string line;while(std::getline(mounts,line)){std::istringstream fields(line);std::string device,point,type,options;if(fields>>device>>point>>type>>options&&point=="/media")noexec=(","+options+",").find(",noexec,")!=std::string::npos;}
        check(noexec,"worker source fixture is mounted noexec");
        pmu::test::SigningFixture key;const auto publisher=key.publicKey(),architecture=std::string(managerArchitecture());const auto input=root/"input";fs::create_directories(input);
        {std::ifstream self("/proc/self/exe",std::ios::binary);std::ofstream target(input/"program",std::ios::binary);target<<self.rdbuf();target.close();check(bool(target),"actual native worker image copied into the fixture");}
        save(input/"supervisor","#!/bin/sh\nif [ \"$1\" = --cancel-prepared ]; then rm -rf \"$2\"; exit 0; fi\nexit 99\n");save(input/"font","font");save(input/"hook","hook");save(input/"launch","#!/bin/sh\nexit 99\n");save(input/"setup","#!/bin/sh\n[ \"$3\" = --arm-only ] || exit 4\numask 077\nprintf 'READY\\n' > \"$2/ready\"\n");
        pmu::GenerationSpec spec{"1.0.0",architecture,{},{{"manager_"+architecture,"program"},{"runtime_worker_"+architecture,"program"},{"supervisor_"+architecture,"supervisor"},{"hook_"+architecture,"hook"},{"font","font"},{"launch","launch"},{"setup","setup"}}};for(const auto& name:{"program","supervisor","font","hook","launch","setup"})spec.files[name]=pmu::fileSha256(input/name);
        save(source/".mpc-manager-id","mpc-usb-manager-v1\n");save(source/"plugin-history.json","preserved");fs::create_directories(source/"autorun/runtime");pmu::VersionStore store(source/"autorun/runtime/.pmu-versions",geteuid(),[](const auto& path){return pmu::fileSha256(path);},{},true,{},architecture);
        auto proof=[&](const auto& value,const auto& kind){return key.proof(pmu::sha256Text(pmu::generationSpecToJson(value).dump()+'\n'),kind);};const auto first=store.stage(input,spec,proof(spec,"runtime"));store.seed(first);pmu::SourceRuntime current(source,architecture,publisher);const auto selected=current.choose(false);const pmu::IncomingWorker native{selected.worker};
        Json task={{"operation","apply"},{"intent","prepare-runtime"},{"source",source.string()},{"helper",""},{"generation",first},{"source_identity",selected.identity},{"test_publisher",publisher}};
        rejects([&]{pmu::handleRuntimeTask(task,publisher,{},"9.9.9");},"a compiled worker version must match its signed runtime metadata");
        const auto prepared=pmu::runIncomingWorker(native,root,task);const fs::path cohort=prepared.at("session").get<std::string>();check(prepared.at("generation")==first&&read(cohort/"ready")=="READY\n","verified native worker prepares a noexec source through the frozen transport");
        char guiPattern[]="/tmp/mpc-usb-manager.worker.XXXXXX";const auto guiName=mkdtemp(guiPattern);check(guiName!=nullptr,"private GUI cache created");const fs::path gui=guiName,cache=gui/"cache";fs::create_directory(cache);
        const auto state=fs::path("/run/mpc-usb-manager-session"),dropin=fs::path("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf");save(state,cohort.string()+'\n'+source.string()+'\n');save(dropin,"[Service]\nEnvironmentFile="+(cohort/"shortcut.env").string()+"\n");save(cohort/"shortcut.env","LD_PRELOAD=\"/existing-addin.so\"\n");
        save(gui/"runtime-context.json",read(cohort/"runtime-context.json"));
        for(const std::string bad:{"source","identity","generation","helper","intent"}){
            auto rejected=task;if(bad=="source")rejected["source"]="/etc";if(bad=="identity")rejected["source_identity"]=selected.identity+"1";if(bad=="generation")rejected["generation"]=std::string(64,'0');if(bad=="helper")rejected["helper"]="/tmp/unowned-helper";if(bad=="intent")rejected["intent"]="unknown";
            rejects([&]{pmu::runIncomingWorker(native,root,rejected);},"unsafe native worker requests are refused");check(store.current()->id==first&&!store.current()->pending,"refused native requests leave the working selection intact");
        }
        const auto bundle=root/"bundle",runtime=bundle/"future/runtime/v3";fs::create_directories(runtime);auto next=spec;next.version="3.0.0";
        for(const auto& item:next.files){fs::copy_file(input/item.first,runtime/item.first);chmod((runtime/item.first).c_str(),0600);}
        save(bundle/"metadata/runtime.json",pmu::generationSpecToJson(next).dump()+'\n');save(bundle/"metadata/runtime-approval.json",proof(next,"runtime"));const auto recipe=Json{{"schema",1},{"product","plugin-manager-usb"},{"runtime",{{"directory","future/runtime/v3"},{"manifest","metadata/runtime.json"},{"approval","metadata/runtime-approval.json"}}},{"helper",nullptr}};save(bundle/"description/install.json",recipe.dump());
        pmu::GenerationSpec outer{"3.0.0",architecture,{},{{"update_worker_"+architecture,"future/runtime/v3/program"},{"deployment","description/install.json"}}};for(const auto& entry:fs::recursive_directory_iterator(bundle))if(entry.is_regular_file())outer.files[entry.path().lexically_relative(bundle).generic_string()]=pmu::fileSha256(entry.path());save(bundle/"PMU-UPDATE.json",pmu::updateEnvelope(outer,proof(outer,"update")).dump());
        auto install=task;install["intent"]="install-update";install["bundle"]=bundle.string();install["version"]="3.0.0";install["session"]=cache.string();
        rejects([&]{pmu::handleRuntimeTask(install,publisher,{},"9.9.9");},"incoming release metadata cannot mislabel a differently compiled update worker");
        rejects([&]{pmu::handleRuntimeTask(install,publisher,{}, {},[](const auto&){throw std::runtime_error("MPC resumed");});},"a resumed MPC application prevents update activation");check(store.current()->id==first&&!store.current()->pending,"a resumed MPC leaves the working generation unchanged");
        const auto result=pmu::runIncomingWorker(native,root,install);check(result.at("version")=="3.0.0"&&store.current()->spec.version=="3.0.0"&&store.current()->pending,"actual incoming native worker verifies and activates the complete signed update");check(read(source/"plugin-history.json")=="preserved","native update leaves user installation history untouched");
        const auto capsule=pmu::queuedSessionHandoff(cache);const auto handoff=pmu::readSessionHandoff(capsule);check(result.at("handoff_queued")==true&&handoff.oldBase==cohort&&handoff.previousGeneration==first,"native update queues a handover tied to the actual GUI session");check(!fs::exists(handoff.newBase/"ready")&&!fs::exists(handoff.rollbackBase/"ready"),"new and retained recovery cohorts remain inactive while the GUI is open");check(read(handoff.newBase/"preload.snapshot")==read(cohort/"shortcut.env")&&read(handoff.rollbackBase/"preload.snapshot")==read(cohort/"shortcut.env"),"both prepared cohorts preserve existing addin ordering");
        check(current.choose(true).generation.id==result.at("generation").get<std::string>(),"native worker result selects the actual pending generation");current.confirm(result.at("generation").get<std::string>(),selected.identity);
        auto retained=task;retained["intent"]="prepare-retained";retained["stage_only"]=true;const auto recovery=pmu::runIncomingWorker(native,root,retained);const fs::path retainedBase=recovery.at("session").get<std::string>();check(recovery.at("generation")==first&&!fs::exists(retainedBase/"ready")&&store.current()->spec.version=="3.0.0","a retained native worker can stage recovery without selecting or starting its older version");fs::remove_all(retainedBase);
        retained["stage_only"]=false;rejects([&]{pmu::runIncomingWorker(native,root,retained);},"retained staging cannot start an old supervisor before explicit recovery");
        auto wrong=install;wrong["version"]="4.0.0";rejects([&]{pmu::runIncomingWorker(native,root,wrong);},"native update refuses a different selected release");
        save(runtime/"hook","tampered");rejects([&]{pmu::runIncomingWorker(native,root,install);},"native update revalidates staged files before any writes");check(store.current()->spec.version=="3.0.0"&&!store.current()->pending,"late tampering preserves the working updated version");
        fs::remove(state);fs::remove(dropin);fs::remove_all(handoff.newBase);fs::remove_all(handoff.rollbackBase);fs::remove_all(cohort);fs::remove_all(gui);fs::remove_all(source);fs::remove_all(root);std::cout<<"PASS: "<<checks<<" runtime-worker noexec startup, signed migration and refused-request checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixtures retained: "<<source<<" and "<<root<<'\n';return 1;}
}
