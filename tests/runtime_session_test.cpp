#include "runtime_session.hpp"
#include "checksum.hpp"
#include "crypto.hpp"
#include "platform.hpp"
#include "signing_fixture.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream output(path);output<<value;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write runtime session fixture");}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Missing runtime session fixture");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(){
    char pattern[]="/tmp/pmu-runtime-session.XXXXXX";const auto made=mkdtemp(pattern);if(!made)return 1;const fs::path root=made;
    try{
        const auto output=pmu::runtimeOutput({"/bin/sh","-c","printf 'loaded\\n'"});check(output.first==0&&output.second=="loaded\n","runtime queries capture their bounded standard output");const auto failure=pmu::runtimeOutput({"/bin/sh","-c","printf 'not-found\\n'; exit 4"});check(failure.first==4&&failure.second=="not-found\n","runtime queries retain explicit nonzero service-query status");rejects([&]{pmu::runtimeOutput({"/bin/sh","-c","head -c 100000 /dev/zero"});},"oversized runtime query output is refused");
        const std::string architecture=managerArchitecture();pmu::test::SigningFixture key;const auto source=root/"USB",input=root/"input";save(source/".mpc-manager-id","mpc-usb-manager-v1\n");save(source/"plugin-history.json","preserved");fs::create_directories(source/"autorun/runtime");
        const std::map<std::string,std::string> contents={
            {"new/bin/editor","native manager fixture"},{"new/bin/supervisor","#!/bin/sh\nif [ \"$1\" = --cancel-prepared ]; then rm -rf \"$2\"; exit 0; fi\nexit 99\n"},
            {"new/lib/hook","hook fixture"},{"new/assets/font","font fixture"},
            {"new/scripts/launch","#!/bin/sh\nexit 99\n"},
            {"new/scripts/setup","#!/bin/sh\n[ \"$3\" = --arm-only ] || [ \"$3\" = --resume-only ] || exit 3\numask 077\nprintf '%s\\n' \"$3\" > \"$2/setup-mode\"\nprintf 'READY\\n' > \"$2/ready\"\n"}};
        pmu::GenerationSpec spec{"2.0.0",architecture,{},{{"manager_"+architecture,"new/bin/editor"},{"runtime_worker_"+architecture,"new/bin/editor"},{"supervisor_"+architecture,"new/bin/supervisor"},{"hook_"+architecture,"new/lib/hook"},{"font","new/assets/font"},{"launch","new/scripts/launch"},{"setup","new/scripts/setup"}}};
        for(const auto& item:contents){save(input/item.first,item.second);spec.files[item.first]=pmu::fileSha256(input/item.first);}
        pmu::VersionStore store(source/"autorun/runtime/.pmu-versions",geteuid(),[](const auto& path){return pmu::fileSha256(path);},{},true,{},architecture);
        auto stage=[&](const auto& value){const auto id=pmu::sha256Text(pmu::generationSpecToJson(value).dump()+'\n');return store.stage(input,value,key.proof(id,"runtime"));};const auto id=stage(spec);store.seed(id);pmu::SourceRuntime current(source,architecture,key.publicKey());const auto selected=current.choose(false);
        auto base=pmu::prepareRuntimeSession(selected,source,{},key.publicKey());check(base.parent_path()=="/tmp"&&base.filename().string().rfind("mpc-usb-session.",0)==0,"runtime uses a private RAM cohort independent of source layout");
        for(const auto& item:std::map<std::string,std::string>{{"manager","new/bin/editor"},{"shortcut","new/bin/supervisor"},{"shortcut-hook.so","new/lib/hook"},{"font.ttf","new/assets/font"},{"launch.sh","new/scripts/launch"},{"setup.sh","new/scripts/setup"}})check(read(base/item.first)==contents.at(item.second),"signed role maps to the RAM cohort without fixing the source filename");
        const auto context=nlohmann::json::parse(read(base/"runtime-context.json"));check(context.at("generation")==id&&context.at("source_identity")==selected.identity&&context.at("worker")=="manager","session binds its exact source, version and native worker");check(read(base/"setup-mode")=="--arm-only\n"&&!fs::exists(base/"preload.snapshot"),"initial preparation stays quiet and receives no inherited preload");
        check(!nlohmann::json::parse(read(source/"autorun/runtime/.pmu-versions"/("active."+architecture+".json"))).at("attempted").get<bool>(),"RAM preparation does not claim GUI startup");fs::remove_all(base);
        const std::string snapshot="LD_PRELOAD=\"/first.so /second.so\"\n";base=pmu::prepareRuntimeSession(selected,source,{},key.publicKey(),{},snapshot);struct stat status{};check(read(base/"preload.snapshot")==snapshot&&stat((base/"preload.snapshot").c_str(),&status)==0&&(status.st_mode&0777)==0600,"handover preserves an owner-only addin snapshot");check(read(base/"setup-mode")=="--resume-only\n","handover prepares while MPC remains stopped");fs::remove_all(base);
        base=pmu::prepareRuntimeSession(selected,source,{},key.publicKey(),{},snapshot,pmu::RuntimeStart::StageOnly);check(!fs::exists(base/"ready")&&!fs::exists(base/"setup-mode"),"deferred cohort staging does not replace the running supervisor");check(pmu::preparedRuntimeWorker(base,key.publicKey()).executable==base/"manager","deferred worker comes from the verified prepared cohort");pmu::startPreparedRuntime(base,key.publicKey());check(read(base/"setup-mode")=="--resume-only\n"&&read(base/"ready")=="READY\n","deferred cohort starts only on explicit handover");save(base/"shortcut-hook.so","changed");rejects([&]{pmu::preparedRuntimeWorker(base,key.publicKey());},"late RAM payload corruption is refused before handover");fs::remove_all(base);
        rejects([&]{pmu::prepareRuntimeSession(selected,source,root/"unsupported-helper",key.publicKey());},"arbitrary helper installation paths are refused");
        auto stale=selected;stale.identity+='1';rejects([&]{pmu::prepareRuntimeSession(stale,source,{},key.publicKey());},"another media identity cannot prepare a session");
        auto wrong=selected;wrong.generation.id=std::string(64,'0');rejects([&]{pmu::prepareRuntimeSession(wrong,source,{},key.publicKey());},"stale generation requests cannot prepare a session");
        auto missing=spec;missing.entrypoints.erase("font");missing.version="3.0.0";const auto second=stage(missing);store.activate(second);const auto selectedMissing=current.choose(false);rejects([&]{pmu::prepareRuntimeSession(selectedMissing,source,{},key.publicKey());},"incomplete session role sets are refused before startup");store.reject(second);
        check(read(source/"plugin-history.json")=="preserved","preparation and refusals preserve user inventory");fs::remove_all(root);std::cout<<"PASS: "<<checks<<" runtime-session signed-role, readiness and handover preparation checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
