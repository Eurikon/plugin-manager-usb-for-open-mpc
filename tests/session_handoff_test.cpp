#include "session_handoff.hpp"
#include "platform.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& value,mode_t mode=0600){fs::create_directories(path.parent_path());std::ofstream output(path);output<<value;output.close();if(!output||chmod(path.c_str(),mode))throw std::runtime_error("Cannot write handoff fixture");}
std::string read(const fs::path& path){std::ifstream input(path);if(!input)throw std::runtime_error("Missing handoff fixture");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(){
    char oldPattern[]="/tmp/mpc-usb-session.old.XXXXXX",newPattern[]="/tmp/mpc-usb-session.new.XXXXXX",rollbackPattern[]="/tmp/mpc-usb-session.rollback.XXXXXX",guiPattern[]="/tmp/mpc-usb-manager.test.XXXXXX";const auto oldName=mkdtemp(oldPattern),newName=mkdtemp(newPattern),rollbackName=mkdtemp(rollbackPattern),guiName=mkdtemp(guiPattern);if(!oldName||!newName||!rollbackName||!guiName)return 1;const fs::path oldBase=oldName,newBase=newName,rollbackBase=rollbackName,work=guiName,cache=work/"cache",source="/media/PM USB";const std::string id(64,'a'),next(64,'b'),identity="1:2:3:4:5";const pmu::NativeImageSpec rollbackWorker{rollbackBase/"manager",id,managerArchitecture()};
    try{
        fs::create_directory(cache);const auto state=fs::path("/run/mpc-usb-manager-session"),dropin=fs::path("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf");
        save(state,oldBase.string()+'\n'+source.string()+'\n');save(dropin,"[Service]\nEnvironmentFile="+(oldBase/"shortcut.env").string()+"\n",0644);save(dropin.parent_path()/"20-foreign.conf","foreign configuration",0644);
        save(oldBase/"runtime-context.json",nlohmann::json({{"protocol",1},{"source",source.string()},{"generation",id},{"source_identity",identity}}).dump());
        const std::string preload="LD_PRELOAD=\"/first.so /second.so $(literal)\"\n";save(oldBase/"shortcut.env",preload);
        check(pmu::queuedSessionHandoff(cache).empty(),"a normal GUI session has no pending handover");
        {
            pmu::UpdateSession session(source,cache,id,identity);check(session.preloadSnapshot()==preload,"snapshot preserves literal addin order without evaluation");rejects([&]{pmu::UpdateSession duplicate(source,cache,id,identity);},"concurrent update workers cannot publish into one GUI session");
            const auto capsule=session.publish(newBase,{},next,rollbackBase,rollbackWorker);check(pmu::queuedSessionHandoff(cache)==capsule,"complete handover reference publishes atomically");const auto parsed=pmu::readSessionHandoff(capsule);check(parsed.source==source&&parsed.oldBase==oldBase&&parsed.newBase==newBase&&parsed.rollbackBase==rollbackBase&&parsed.previousGeneration==id&&parsed.rollbackWorker.executable==rollbackWorker.executable&&parsed.generation==next&&parsed.sourceIdentity==identity&&parsed.service=="acvs"&&!parsed.openAfterHandoff,"handover binds the source, active and recovery cohorts, release and service");
            check(read(dropin.parent_path()/"20-foreign.conf")=="foreign configuration"&&read(oldBase/"shortcut.env")==preload,"queuing never changes active or foreign service configuration");
        }
        rejects([&]{pmu::UpdateSession repeated(source,cache,id,identity);},"another update waits until the first handover completes");fs::remove(cache/"manager-update.pending");fs::remove(rollbackBase/"update-handoff.json");
        rejects([&]{pmu::UpdateSession stale(source,cache,next,identity);},"stale cohort versions cannot authorize handover");rejects([&]{pmu::UpdateSession wrong(source,cache,id,identity+"1");},"another source identity cannot authorize handover");rejects([&]{pmu::UpdateSession wrong("/media/another",cache,id,identity);},"a different source cannot claim the running service");
        chmod((oldBase/"shortcut.env").c_str(),0644);rejects([&]{pmu::UpdateSession exposed(source,cache,id,identity);},"exposed snapshots are refused");chmod((oldBase/"shortcut.env").c_str(),0600);
        fs::rename(oldBase/"shortcut.env",oldBase/"saved.env");fs::create_symlink(oldBase/"saved.env",oldBase/"shortcut.env");rejects([&]{pmu::UpdateSession linked(source,cache,id,identity);},"linked snapshots are refused");fs::remove(oldBase/"shortcut.env");fs::rename(oldBase/"saved.env",oldBase/"shortcut.env");
        {
            pmu::UpdateSession changed(source,cache,id,identity);save(state,oldBase.string()+"\n/media/other\n");rejects([&]{changed.publish(newBase,{},next,rollbackBase,rollbackWorker);},"late service ownership changes prevent publication");check(!fs::exists(cache/"manager-update.pending")&&!fs::exists(rollbackBase/"update-handoff.json"),"late ownership refusal creates no handover state");save(state,oldBase.string()+'\n'+source.string()+'\n');
        }
        {
            pmu::UpdateSession session(source,cache,id,identity);const auto capsule=session.publish(newBase,{},next,rollbackBase,rollbackWorker,{},{},true);check(pmu::readSessionHandoff(capsule).openAfterHandoff,"recovery may request reopening only through the explicit handover flag");save(capsule,"{}");rejects([&]{pmu::readSessionHandoff(capsule);},"truncated handover capsules are refused");fs::remove(cache/"manager-update.pending");save(cache/"manager-update.pending","/etc/passwd\n");rejects([&]{pmu::queuedSessionHandoff(cache);},"queued references cannot leave private runtime cohorts");
        }
        fs::remove(state);fs::remove(dropin);fs::remove(dropin.parent_path()/"20-foreign.conf");fs::remove_all(oldBase);fs::remove_all(newBase);fs::remove_all(rollbackBase);fs::remove_all(work);std::cout<<"PASS: "<<checks<<" session-handoff ownership, serialization and atomic publication checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
