#include "helper_controller_update.hpp"
#include "session_handoff.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "platform.hpp"
#include "startup/storage.h"
#include "signing_fixture.hpp"
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
size_t checks;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class F> void rejects(F operation,const char* message){try{operation();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write controller fixture");}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Missing controller fixture");return {std::istreambuf_iterator<char>(input),{}};}
fs::path temporary(const std::string& prefix){std::string pattern="/tmp/"+prefix+".XXXXXX";std::vector<char> text(pattern.begin(),pattern.end());text.push_back(0);const auto made=mkdtemp(text.data());if(!made)throw std::runtime_error("Cannot create controller fixture");return made;}
struct Fixture {
    fs::path root,recovery,helper=PMU_HELPER_DIRECTORY,controller;
    pmu::test::SigningFixture key;
    pmu::UpdateBundle bundle;
    std::string previous,replacement;
    Fixture(const fs::path& old,const fs::path& next):root(temporary("pmu-controller-test")),recovery(temporary("mpc-usb-session.controller")),controller(helper/(std::string(::managerArchitecture())=="aarch64"?"manager-entry-aarch64.so":"manager-entry.so")){
        check(!fs::exists(helper),"controller fixture refuses to overwrite an existing helper");fs::create_directories(helper);chmod(helper.c_str(),0700);
        fs::copy_file(old,controller);chmod(controller.c_str(),0755);previous=pmu::fileSha256(controller);save(helper/"preferences.json","user preferences");
        const auto input=root/"bundle";fs::create_directory(input);chmod(input.c_str(),0700);fs::copy_file(next,input/"controller.so");fs::copy_file("/proc/self/exe",input/"worker");
        replacement=pmu::fileSha256(input/"controller.so");save(input/"policy.json",Json({{"schema",1},{"abi",1},{"previous",{{std::string(::managerArchitecture()),Json::array({previous})}}}}).dump());
        pmu::GenerationSpec spec{"2.0.0",std::string(::managerArchitecture()),{{"controller.so",replacement},{"worker",pmu::fileSha256(input/"worker")},{"policy.json",pmu::fileSha256(input/"policy.json")}},{{"controller_v1_"+std::string(::managerArchitecture()),"controller.so"},{"controller_policy","policy.json"},{"update_worker_"+std::string(::managerArchitecture()),"worker"}}};
        const auto id=pmu::sha256Text(pmu::generationSpecToJson(spec).dump()+'\n');save(input/"PMU-UPDATE.json",pmu::updateEnvelope(spec,key.proof(id,"update")).dump());bundle=pmu::verifyStagedUpdate(input,std::string(::managerArchitecture()),"2.0.0",key.publicKey());
    }
    ~Fixture(){fs::remove_all(helper);fs::remove_all(root);fs::remove_all(recovery);}
    bool stage(const pmu::VersionStore::Checkpoint& checkpoint={}){return pmu::stageControllerUpdate(bundle,helper,recovery,key.publicKey(),checkpoint);}
    void preserved(){check(read(helper/"preferences.json")=="user preferences","controller update preserves unrelated helper files");}
    fs::path capsule(){
        fs::copy_file("/proc/self/exe",recovery/"old-worker");const auto hash=pmu::fileSha256(recovery/"old-worker");
        const auto path=recovery/"update-handoff.json";save(path,Json({{"protocol",1},{"source","/media/controller-test"},{"helper",helper.string()},{"old_base","/tmp/mpc-usb-session.old-controller"},{"new_base","/tmp/mpc-usb-session.new-controller"},{"rollback_base",recovery.string()},{"generation",std::string(64,'a')},{"previous_generation",std::string(64,'b')},{"helper_generation",""},{"previous_helper_generation",""},{"source_identity","1:2:3"},{"service","acvs"},{"rollback_worker",(recovery/"old-worker").string()},{"rollback_worker_sha256",hash},{"open_after_handoff",false}}).dump());return path;
    }
};
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--pmu-recover-v1"){const auto item=pmu::readSessionHandoff(argv[2]);save(item.rollbackBase/"old-worker-called","yes");return 0;}
    if(argc==3&&std::string(argv[1])=="--pmu-controller-recover-v1")return pmu::recoverControllerHandoff(argv[2]);
    check(argc==3&&std::getenv("PMU_CONTROLLER_FIXTURE")&&std::string(std::getenv("PMU_CONTROLLER_FIXTURE"))=="1","controller tests require an explicit offline fixture");
    const fs::path old=fs::canonical(argv[1]),next=fs::canonical(argv[2]);
    {
        Fixture f(old,next);const auto before=pmu::fileSha256(f.controller);check(f.stage(),"verified update stages an approved controller replacement");check(pmu::fileSha256(f.controller)==before,"staging does not replace the installed controller");
        pmu::applyControllerUpdate(f.recovery);check(pmu::fileSha256(f.controller)==f.replacement,"controller publication installs the verified replacement");check(pmu::controllerRecovery(f.recovery).has_value(),"controller migration retains its independent recovery worker");
        pmu::rollbackControllerUpdate(f.recovery);check(pmu::fileSha256(f.controller)==f.previous,"failed migration restores the exact previous controller");f.preserved();
    }
    for(const std::string defect:{"publisher","payload","previous","symlink","collision"}){
        Fixture f(old,next);
        if(defect=="payload")save(f.bundle.directory/"controller.so","changed");
        if(defect=="previous"){std::ofstream output(f.controller,std::ios::app);output<<'x';}
        if(defect=="symlink"){fs::copy_file(f.controller,f.root/"preserved-controller");fs::remove(f.controller);fs::create_symlink(f.root/"preserved-controller",f.controller);}
        if(defect=="collision")save(f.recovery/"controller-new.so","unrelated file");
        const auto before=read(f.controller);rejects([&]{if(defect=="publisher")pmu::stageControllerUpdate(f.bundle,f.helper,f.recovery,std::string(64,'0'));else f.stage();},"untrusted, changed or conflicting controller migrations are refused");check(read(f.controller)==before,"refused controller migration preserves installed bytes");f.preserved();
    }
    {
        Fixture f(old,next);f.stage();save(f.controller,"external modification");rejects([&]{pmu::applyControllerUpdate(f.recovery);},"publication refuses an externally modified controller");check(read(f.controller)=="external modification","external controller changes are preserved");
    }
    {
        Fixture f(old,next);f.stage();save(f.recovery/"controller-new.so","damaged replacement");rejects([&]{pmu::applyControllerUpdate(f.recovery);},"changed private replacement cannot be published");check(pmu::fileSha256(f.controller)==f.previous,"changed private copy leaves the original controller active");
    }
    for(const std::string point:{"controller-copy-durable","controller-replaced"}){
        Fixture f(old,next);f.stage();const auto child=fork();check(child>=0,"controller interruption fixture forked");if(!child){pmu::applyControllerUpdate(f.recovery,[&](const auto& current){if(current==point)kill(getpid(),SIGKILL);});_exit(90);}int status;check(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,"controller migration interrupted at its durable boundary");
        const auto hash=pmu::fileSha256(f.controller);check(hash==f.previous||hash==f.replacement,"interrupted migration retains a complete old or new controller");pmu::rollbackControllerUpdate(f.recovery);check(pmu::fileSha256(f.controller)==f.previous&&!fs::exists(f.helper/".pmu-controller-update"),"controller recovery also clears its interrupted staging file");f.preserved();
    }
    for(bool success:{false,true}){
        Fixture f(old,next);f.stage();pmu::applyControllerUpdate(f.recovery);const auto capsule=f.capsule();if(success)save(f.recovery/"handoff.complete",std::string(64,'a')+'\n');
        check(pmu::recoverControllerHandoff(capsule)==0,"controller guard delegates to the original v1 recovery worker");check(pmu::fileSha256(f.controller)==(success?f.replacement:f.previous),"controller guard commits only a completed handover");check(read(f.recovery/"old-worker-called")=="yes","original recovery protocol remains callable");f.preserved();
    }
    {
        Fixture f(old,next);fs::copy_file(next,f.controller,fs::copy_options::overwrite_existing);check(!f.stage()&&!pmu::controllerRecovery(f.recovery),"already current controller needs no replacement or recovery guard");
    }
    std::cout<<"PASS: "<<checks<<" helper-controller signed migration, atomic replacement and recovery checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
