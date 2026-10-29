#include "runtime_worker.hpp"
#include "runtime_session.hpp"
#include "migration_package.hpp"
#include "session_handoff.hpp"
#include "runtime_handoff.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "platform.hpp"
#include "startup/storage.h"
#include <fcntl.h>
#include <limits.h>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
fs::path pathField(const Json& task,const std::string& name,bool optional=false){
    const auto text=task.value(name,std::string());if(optional&&text.empty())return {};
    const fs::path value=text;need(!text.empty()&&text.size()<PATH_MAX&&text.find('\0')==std::string::npos&&text.find_first_of("\r\n")==std::string::npos&&value.is_absolute()&&value.lexically_normal()==value,"Invalid runtime task path");return value;
}
void sourceLocation(const fs::path& source){
    const auto relative=source.lexically_relative("/media");need(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="."&&*relative.begin()!=".."&&*relative.begin()!="az01-internal","Manager source is outside removable storage");
    auto part=relative.begin();++part;if(part!=relative.end()){need(*part=="Plugin Manager USB","Unsupported manager source subfolder");++part;need(part==relative.end(),"Unsupported manager source depth");}
}
void helperLocation(const fs::path& helper){
    if(helper.empty())return;
    need(pmu_helper_directory_valid(helper.c_str()),"Unsupported helper task location");
#if !defined(__aarch64__)
    need(helper==PMU_HELPER_DIRECTORY,"Unsupported Gen 1 helper task location");
#endif
}
std::string selfHash(){
    const int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);need(fd>=0,"Cannot identify the running update worker");
    try{const auto hash=fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd));close(fd);return hash;}catch(...){close(fd);throw;}
}
void pruneWorking(const fs::path& path,const std::string& profile,const std::string& kind,const std::string& publisher){
    try{VersionStore store(path,geteuid(),[](const auto& file){return fileSha256(file);},{},false,[&](const auto& id,const auto&,const auto& proof){verifyAuthorization(publisher,kind,id,proof);},profile);const auto current=store.current();if(current&&!current->pending)store.prune();}
    catch(const std::exception& error){std::fprintf(stderr,"Old manager versions preserved: %.1024s\n",error.what());}
}
}
Json handleRuntimeTask(const Json& task,const std::string& publisher,const UpdateProgress& progress,const std::string& runningVersion,const std::function<void(const std::string&)>& stoppedCheck){
    need(geteuid()==0&&task.is_object()&&task.value("operation",std::string())=="apply","Runtime task requires the manager service");
    need(task.value("publisher",publisher)==publisher,"Worker publisher differs from the requested authority");
    const auto source=pathField(task,"source"),helper=pathField(task,"helper",true);sourceLocation(source);helperLocation(helper);
    const auto identity=task.at("source_identity").get<std::string>();need(!identity.empty()&&identity.size()<=128,"Invalid runtime task source identity");const std::string architecture=managerArchitecture();
    const auto intent=task.at("intent").get<std::string>();need(intent=="prepare-runtime"||intent=="prepare-retained"||intent=="install-update"||intent=="finish-update"||intent=="refresh-runtime","Unsupported runtime task intent");
    SourceRuntime current(source,architecture,publisher);const auto selected=current.choose(false,identity);
    if(intent=="refresh-runtime"){
        need(runningVersion.empty()||selected.generation.spec.version==runningVersion,"Running worker version differs from the selected runtime");
        need(selfHash()==selected.worker.sha256,"A different running program cannot refresh this runtime");const auto cache=pathField(task,"session");const auto context=guiRuntimeContext(cache.parent_path());need(context.at("source")==source.string()&&context.at("source_identity")==identity,"Another GUI session cannot refresh this runtime");const auto previous=context.at("generation").get<std::string>();need(previous!=selected.generation.id,"Runtime refresh does not need a handover");UpdateSession session(source,cache,previous,identity);const auto snapshot=session.preloadSnapshot();const auto retained=current.retained(previous,identity);
        const auto result=runIncomingWorker(retained.worker,cache,{{"operation","apply"},{"intent","prepare-retained"},{"source",source.string()},{"helper",helper.string()},{"generation",previous},{"source_identity",identity},{"publisher",publisher},{"preload_snapshot",snapshot},{"stage_only",true}},progress);const fs::path rollback=result.at("session").get<std::string>();const auto& image=result.at("worker");const NativeImageSpec recovery{image.at("path").get<std::string>(),image.at("sha256").get<std::string>(),image.at("architecture").get<std::string>()};need(result.at("generation")==previous&&result.at("source_identity")==identity&&recovery.executable.parent_path()==rollback&&recovery.architecture==architecture,"Retained worker returned another recovery cohort");validateNativeImage(recovery);
        const auto prepared=prepareRuntimeSession(selected,source,helper,publisher,progress,snapshot,RuntimeStart::StageOnly);const auto capsule=session.publish(prepared,helper,selected.generation.id,rollback,recovery,{},{},true);beginRuntimeHandoff(capsule,publisher);return {{"handoff_started",true}};
    }
    if(intent=="finish-update"){
        need(runningVersion.empty()||selected.generation.spec.version==runningVersion,"Running worker version differs from the selected runtime");
        need(selfHash()==selected.worker.sha256,"A different running program cannot finish this runtime update");const auto capsule=pathField(task,"handoff");const auto item=readSessionHandoff(capsule);need(item.source==source&&item.helper==helper&&item.generation==selected.generation.id&&item.sourceIdentity==identity,"Queued handover does not match this runtime update");beginRuntimeHandoff(capsule,publisher);return {{"handoff_started",true}};
    }
    if(intent=="prepare-runtime"||intent=="prepare-retained"){
        const bool retained=intent=="prepare-retained";const auto requested=retained?current.retained(task.at("generation").get<std::string>(),identity):selected;
        need(runningVersion.empty()||requested.generation.spec.version==runningVersion,"Running worker version differs from the requested runtime");
        need(task.at("generation").get<std::string>()==requested.generation.id,"Stale runtime preparation request");
        need(selfHash()==requested.worker.sha256,"A different running program cannot prepare this runtime version");
        if(!retained&&!requested.generation.pending){pruneWorking(source/"autorun/runtime/.pmu-versions",architecture,"runtime",publisher);if(!helper.empty())pruneWorking(helper/".pmu-versions",{},"helper",publisher);}
        const auto snapshot=task.value("preload_snapshot",std::string());const auto mode=task.value("stage_only",false)?RuntimeStart::StageOnly:RuntimeStart::Quiet;
        need(!retained||mode==RuntimeStart::StageOnly,"Retained recovery cannot replace the active runtime before restoration");const auto base=prepareRuntimeSession(requested,source,helper,publisher,progress,snapshot,mode,retained);const auto worker=preparedRuntimeWorker(base,publisher,retained);
        return {{"session",base.string()},{"generation",requested.generation.id},{"source_identity",requested.identity},{"worker",{{"path",worker.executable.string()},{"sha256",worker.sha256},{"architecture",worker.architecture}}}};
    }
    if(intent=="install-update"){
        const auto directory=pathField(task,"bundle");const auto relative=directory.lexically_relative("/tmp");need(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Incoming update is outside private RAM staging");
        const auto version=task.at("version").get<std::string>();need(runningVersion.empty()||version==runningVersion,"Incoming update worker has a different compiled version");const auto bundle=verifyStagedUpdate(directory,architecture,version,publisher);
        const auto worker=bundle.spec.entrypoints.at("update_worker_"+architecture);need(selfHash()==bundle.spec.files.at(worker),"A different running program cannot apply this update");
        const auto plan=readMigrationPackage(bundle,architecture);const auto cache=pathField(task,"session");const auto context=guiRuntimeContext(cache.parent_path());need(context.at("source")==source.string()&&context.at("helper")==helper.string()&&context.at("generation")==selected.generation.id&&context.at("source_identity")==identity,"The update request does not match the running GUI cohort");UpdateSession session(source,cache,selected.generation.id,identity);
        auto stopped=[&]{if(stoppedCheck)stoppedCheck(session.mpcService());else{const auto state=runtimeOutput({"/bin/systemctl","show",session.mpcService(),"-pMainPID","--value"});need(state.first==0&&state.second=="0\n","MPC must remain stopped while the manager update is prepared");}};stopped();
        const auto snapshot=session.preloadSnapshot();const auto rollback=runIncomingWorker(selected.worker,cache,{{"operation","apply"},{"intent","prepare-runtime"},{"source",source.string()},{"helper",helper.string()},{"generation",selected.generation.id},{"source_identity",identity},{"publisher",publisher},{"preload_snapshot",snapshot},{"stage_only",true}},progress);
        const fs::path rollbackBase=rollback.at("session").get<std::string>();const auto& rollbackImage=rollback.at("worker");const NativeImageSpec rollbackWorker{rollbackImage.at("path").get<std::string>(),rollbackImage.at("sha256").get<std::string>(),rollbackImage.at("architecture").get<std::string>()};need(rollback.at("generation")==selected.generation.id&&rollback.at("source_identity")==identity&&rollbackWorker.executable.parent_path()==rollbackBase&&rollbackWorker.architecture==architecture,"Recovery worker returned another runtime identity");validateNativeImage(rollbackWorker);
        MigrationSelection result;fs::path prepared;bool queued=false;
        try{
            stopped();result=activateMigration(source,helper,identity,architecture,publisher,plan.runtime,plan.helper,progress,{},selected.generation.id);
            const auto next=current.choose(false,identity);need(next.generation.id==result.runtime,"Updated manager selection changed before handover staging");prepared=prepareRuntimeSession(next,source,helper,publisher,progress,snapshot,RuntimeStart::StageOnly);
            stopped();const auto capsule=session.publish(prepared,helper,result.runtime,rollbackBase,rollbackWorker,result.helper,result.previousHelper);queued=true;
            return {{"generation",result.runtime},{"helper_generation",result.helper},{"version",version},{"handoff",capsule.string()},{"handoff_queued",true}};
        }catch(...){
            if(!queued){
                auto trust=[&](const std::string& kind){return [publisher,kind](const auto& id,const auto&,const auto& proof){verifyAuthorization(publisher,kind,id,proof);};};
                if(!result.runtime.empty())try{VersionStore store(source/"autorun/runtime/.pmu-versions",geteuid(),[](const auto& file){return fileSha256(file);},{},false,trust("runtime"),architecture);store.restore(result.previousRuntime,result.runtime);}catch(...){}
                if(!result.helper.empty())try{VersionStore store(helper/".pmu-versions",geteuid(),[](const auto& file){return fileSha256(file);},{},false,trust("helper"));store.restore(result.previousHelper,result.helper);}catch(...){}
                std::error_code ignored;if(!prepared.empty())fs::remove_all(prepared,ignored);fs::remove_all(rollbackBase,ignored);
            }
            throw;
        }
    }
    throw std::runtime_error("Unsupported runtime task intent");
}
}
