#include "update_migration.hpp"
#include "source_runtime.hpp"
#include "checksum.hpp"
#include "crypto.hpp"
#include <stdexcept>
#include <unistd.h>

namespace pmu {
namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void supported(const MigrationPayload& payload,const std::string& architecture,const std::string& kind){
    need(payload.specification.architecture==architecture||payload.specification.architecture=="universal","Update payload does not support this device generation");
    const auto& roles=payload.specification.entrypoints;
    if(kind=="runtime")need(roles.count("runtime_worker_"+architecture)&&roles.count("manager_"+architecture),"Update has no complete native runtime");
    else need(roles.count("helper_"+architecture)&&roles.count("bootstrap"),"Update has no complete native helper");
}
}
MigrationSelection activateMigration(const std::filesystem::path& source,const std::filesystem::path& helper,
    const std::string& identity,const std::string& architecture,const std::string& publisher,
    const MigrationPayload& runtime,const std::optional<MigrationPayload>& payload,
    const UpdateProgress& progress,const VersionStore::Checkpoint& checkpoint,const std::string& expected){
    need(!identity.empty(),"Update needs the active manager source identity");
    SourceRuntime current(source,architecture,publisher);
    const auto before=current.choose(false,identity);
    need(expected.empty()||before.generation.id==expected,"Manager selection changed before update staging");
    need(!before.generation.pending,"Confirm or recover the current manager before another update");
    supported(runtime,architecture,"runtime");
    auto hash=[](const auto& path){return fileSha256(path);};
    auto trust=[&](const std::string& kind){return [publisher,kind](const auto& id,const auto&,const auto& proof){verifyAuthorization(publisher,kind,id,proof);};};
    auto points=[&](const std::string& kind){return [checkpoint,kind](const auto& point){if(checkpoint)checkpoint(kind+":"+point);};};
    VersionStore runtimeStore(source/"autorun/runtime/.pmu-versions",geteuid(),hash,points("runtime"),false,trust("runtime"),architecture);
    if(progress.message)progress.message("Verifying and staging the complete manager version...");
    const auto runtimeId=runtimeStore.stage(runtime.directory,runtime.specification,runtime.authorization);
    std::unique_ptr<VersionStore> helperStore;
    std::string helperId,previousHelper;
    if(!helper.empty()){
        need(payload.has_value(),"Update does not contain the installed helper payload");supported(*payload,architecture,"helper");
        helperStore=std::make_unique<VersionStore>(helper/".pmu-versions",geteuid(),hash,points("helper"),false,trust("helper"));
        const auto selected=helperStore->current();need(selected.has_value()&&!selected->pending,"Confirm or recover the current helper before another update");
        previousHelper=selected->id;
        if(progress.message)progress.message("Verifying and staging the complete helper version...");
        helperId=helperStore->stage(payload->directory,payload->specification,payload->authorization);
    }
    current.choose(false,identity);
    if(checkpoint)checkpoint("migration-staged");
    current.choose(false,identity);
    // Either helper version understands the frozen source-worker protocol.
    if(helperStore){helperStore->activate(helperId,previousHelper);if(checkpoint)checkpoint("migration-helper-active");}
    current.choose(false,identity);
    runtimeStore.activate(runtimeId,before.generation.id);
    if(checkpoint)checkpoint("migration-runtime-active");
    need(current.choose(false,identity).generation.id==runtimeId,"Manager source changed during update activation");
    if(runtime.specification.architecture=="universal"){
        const std::string other=architecture=="armv7"?"aarch64":"armv7";const auto record=source/"autorun/runtime/.pmu-versions"/("active."+other+".json");
        if(runtime.specification.entrypoints.count("manager_"+other)&&runtime.specification.entrypoints.count("runtime_worker_"+other)&&std::filesystem::exists(record)){VersionStore alternate(source/"autorun/runtime/.pmu-versions",geteuid(),hash,{},false,trust("runtime"),other);alternate.offer(runtimeId);}
    }
    if(progress.fraction)progress.fraction(1);
    if(progress.message)progress.message("Update prepared. Return to MPC to activate the complete version.");
    return {runtimeId,helperId,before.generation.id,previousHelper};
}
}
