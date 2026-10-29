#include "runtime_handoff.hpp"
#include "runtime_session.hpp"
#include "session_handoff.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "platform.hpp"
#include "signing_fixture.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
fs::path source,newBase,rollbackBase;
std::string imageHash,publisher,newGeneration;
bool failStart=false,failMpc=false;
bool mpcRunning=false,failHelper=false;
unsigned mpcStarts=0;
unsigned mpcStops=0;
unsigned unitQueries=0;
fs::path helperRoot;
std::string oldHelper;
std::vector<std::thread> supervisors;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write handover fixture");}
fs::path make(const char* label){std::string pattern=std::string("/tmp/")+label+".XXXXXX";std::vector<char> value(pattern.begin(),pattern.end());value.push_back(0);const auto path=mkdtemp(value.data());if(!path)throw std::runtime_error("Cannot create handover fixture");return path;}
void supervise(const fs::path& base){
    save("/run/mpc-usb-manager-session",base.string()+'\n'+source.string()+'\n');save("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf","[Service]\nEnvironmentFile="+(base/"shortcut.env").string()+"\n");
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)throw std::runtime_error("Cannot create supervisor fixture socket");sockaddr_un address{};address.sun_family=AF_UNIX;const auto path=(base/"shortcut.sock").string();std::copy(path.begin(),path.end(),address.sun_path);if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof address))throw std::runtime_error("Cannot bind supervisor fixture socket");chmod(path.c_str(),0600);timeval limit{20,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof limit);
    supervisors.emplace_back([fd,base]{char value=0;for(;;){const auto count=recv(fd,&value,1,0);if(count<0&&errno==EINTR)continue;if(count!=1)break;if(value=='H'){fs::remove("/run/mpc-usb-manager-session");fs::remove("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf");fs::remove_all(base);break;}}close(fd);});
}
void join(){for(auto& thread:supervisors)if(thread.joinable())thread.join();supervisors.clear();}
}
namespace pmu {
NativeImageSpec preparedRuntimeWorker(const fs::path& base,const std::string& key,bool){if(key!=publisher||(base!=newBase&&base!=rollbackBase))throw std::runtime_error("Unexpected prepared worker request");return {base/"manager",imageHash,managerArchitecture()};}
void startPreparedRuntime(const fs::path& base,const std::string& key){check(!mpcRunning,"supervisor replacement and recovery keep MPC stopped until the hook is ready");check(unitQueries>=3,"replacement waits until systemd releases the previous transient unit name");if(failStart&&base==newBase)throw std::runtime_error("Injected supervisor readiness failure");SourceRuntime runtime(source,managerArchitecture(),key);if(base==newBase&&runtime.choose(false).generation.id!=newGeneration)throw std::runtime_error("Wrong new runtime selected");supervise(base);unitQueries=0;}
std::pair<int,std::string> runtimeOutput(const std::vector<std::string>& arguments,std::chrono::seconds){if(arguments==std::vector<std::string>({"/bin/systemctl","show","acvs","-pMainPID","--value"}))return {0,mpcRunning?"42\n":"0\n"};check(arguments==std::vector<std::string>({"/bin/systemctl","show","mpc-usb-manager.service","-pLoadState","--value"}),"handover queries only its own transient service");return {0,++unitQueries<3?"loaded\n":"not-found\n"};}
int runtimeCommand(const std::vector<std::string>& arguments,const fs::path&,std::chrono::seconds){
    if(arguments.at(0)=="/usr/bin/systemd-run"){
        const auto capsule=readSessionHandoff(arguments.back());check(arguments.size()==11&&arguments[2]=="--collect"&&arguments[4]=="--property=RuntimeMaxSec=180"&&arguments[6]=="--property=TimeoutStopSec=180"&&arguments[7].find("--pmu-recover-v1")!=std::string::npos&&arguments[7].find(capsule.rollbackWorker.executable.string())!=std::string::npos,"bounded independent recovery runs from the retained previous cohort");save(capsule.rollbackBase/"handoff.accepted",capsule.generation+'\n');return 0;
    }
    if(arguments==std::vector<std::string>({"/bin/systemctl","stop","acvs"})){mpcRunning=false;++mpcStops;return 0;}
    check(arguments==std::vector<std::string>({"/bin/systemctl","start","acvs"}),"handover only starts the identified MPC application service");++mpcStarts;if(failMpc)return 1;mpcRunning=true;if(failHelper){VersionStore helper(helperRoot/".pmu-versions",geteuid(),[](const auto& file){return fileSha256(file);});helper.boot();failHelper=false;}return 0;
}
}
int main(){
    try{
        for(const std::string mode:{"service","success","supervisor-failure","mpc-failure","helper-failure","refresh-failure"}){
            std::cout<<"HANDOFF MODE "<<mode<<std::endl;
            auto root=make("pmu-runtime-handoff"),input=root/"input";source=root/"source";const auto old=make("mpc-usb-session.old");newBase=make("mpc-usb-session.new");rollbackBase=make("mpc-usb-session.rollback");const auto work=make("mpc-usb-manager.handoff"),cache=work/"cache";fs::create_directory(cache);fs::create_directories(input);save(source/".mpc-manager-id","mpc-usb-manager-v1\n");fs::create_directories(source/"autorun/runtime");
            {std::ifstream image("/proc/self/exe",std::ios::binary);std::ofstream copy(input/"manager",std::ios::binary);copy<<image.rdbuf();}chmod((input/"manager").c_str(),0600);imageHash=pmu::fileSha256(input/"manager");for(const auto& base:{newBase,rollbackBase}){fs::copy_file(input/"manager",base/"manager");chmod((base/"manager").c_str(),0700);}
            pmu::test::SigningFixture key;publisher=key.publicKey();const std::string arch=managerArchitecture();pmu::VersionStore store(source/"autorun/runtime/.pmu-versions",geteuid(),[](const auto& path){return pmu::fileSha256(path);},{},true,{},arch);pmu::GenerationSpec spec{"1.0.0",arch,{{"manager",imageHash}},{{"manager_"+arch,"manager"},{"runtime_worker_"+arch,"manager"}}};auto stage=[&]{return store.stage(input,spec,key.proof(pmu::sha256Text(pmu::generationSpecToJson(spec).dump()+'\n'),"runtime"));};const auto previous=stage();store.seed(previous);pmu::SourceRuntime runtime(source,arch,publisher);const auto identity=runtime.identity();spec.version="2.0.0";newGeneration=stage();store.activate(newGeneration);std::string stale=previous;if(mode=="refresh-failure"){stale=newGeneration;store.boot();store.boot();newGeneration=previous;check(store.current()->id==previous&&!store.current()->pending,"failed GUI selection has already returned to its working version");}
            save(old/"runtime-context.json",nlohmann::json({{"protocol",1},{"source",source.string()},{"generation",stale},{"source_identity",identity}}).dump());save(old/"shortcut.env","LD_PRELOAD=\"/foreign.so\"\n");supervise(old);save("/run/systemd/system/acvs.service.d/20-foreign.conf","untouched addin");
            helperRoot.clear();oldHelper.clear();std::string nextHelper;
            if(mode=="helper-failure"){
                helperRoot=PMU_HELPER_DIRECTORY;fs::create_directories(helperRoot);save(input/"helper","original helper");pmu::VersionStore helpers(helperRoot/".pmu-versions",geteuid(),[](const auto& file){return pmu::fileSha256(file);});pmu::GenerationSpec descriptor{"1.0.0",arch,{{"helper",pmu::fileSha256(input/"helper")}}};auto sign=[&]{return helpers.stage(input,descriptor,key.proof(pmu::sha256Text(pmu::generationSpecToJson(descriptor).dump()+'\n'),"helper"));};oldHelper=sign();helpers.seed(oldHelper);descriptor.version="2.0.0";nextHelper=sign();helpers.activate(nextHelper);helpers.boot();
            }
            pmu::UpdateSession session(source,cache,stale,identity);const auto capsule=session.publish(newBase,helperRoot,newGeneration,rollbackBase,{rollbackBase/"manager",imageHash,arch},nextHelper,oldHelper,mode=="refresh-failure");mpcStarts=mpcStops=unitQueries=0;mpcRunning=false;failStart=mode=="supervisor-failure"||mode=="refresh-failure";failMpc=mode=="mpc-failure";failHelper=mode=="helper-failure";
            if(mode=="service"){
                pmu::beginRuntimeHandoff(capsule,publisher);check(fs::exists(rollbackBase/"handoff.accepted")&&mpcStarts==0,"deferred service acceptance does not resume MPC while the GUI is closing");
                fs::remove(rollbackBase/"handoff.accepted");
                pmu::finalizeRuntimeHandoff(capsule,publisher);
            }else if(mode=="success")pmu::finalizeRuntimeHandoff(capsule,publisher);
            else rejects([&]{pmu::finalizeRuntimeHandoff(capsule,publisher);},"a failed supervisor or MPC restart cannot mark the handover successful");
            failMpc=false;pmu::recoverRuntimeHandoff(capsule,publisher);
            if(mode=="success"||mode=="service"){
                check(!fs::exists(rollbackBase)&&store.current()->id==newGeneration,"successful handover retires only the unused recovery cohort");check(mpcStarts==1,"normal handover starts MPC exactly once");
            }else{check(store.current()->id==previous&&!store.current()->pending,"independent recovery restores the previous authenticated runtime selection");check(fs::exists(rollbackBase),"a recovery cohort remains available while it is active");check(mpcStops==1&&mpcRunning,"recovery reloads MPC with the restored helper and shortcut");}
            check(fs::exists("/run/systemd/system/acvs.service.d/20-foreign.conf"),"handover and recovery preserve foreign addin configuration");
            const auto active=(mode=="success"||mode=="service")?newBase:rollbackBase;int socketFd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);sockaddr_un address{};address.sun_family=AF_UNIX;const auto socketPath=(active/"shortcut.sock").string();std::copy(socketPath.begin(),socketPath.end(),address.sun_path);sendto(socketFd,"H",1,0,reinterpret_cast<sockaddr*>(&address),sizeof address);close(socketFd);join();fs::remove("/run/systemd/system/acvs.service.d/20-foreign.conf");if(!helperRoot.empty())fs::remove_all(helperRoot);fs::remove_all(root);fs::remove_all(old);fs::remove_all(newBase);fs::remove_all(rollbackBase);fs::remove_all(work);
        }
        std::cout<<"PASS: "<<checks<<" runtime-handoff deferred service, supervisor replacement and independent recovery checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';for(auto& thread:supervisors)if(thread.joinable())thread.detach();return 1;}
}
