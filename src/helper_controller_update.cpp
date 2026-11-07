#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "helper_controller_update.hpp"
#include "checksum.hpp"
#include "metadata.hpp"
#include "filesystem_identity.hpp"
#include "session_handoff.hpp"
#include "update_worker.hpp"
#include "platform.hpp"
#include "startup/storage.h"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr const char* record="controller-update.json";
constexpr const char* temporary=".pmu-controller-update";
void need(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
bool hex(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other)noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;}
};
std::string descriptor(int fd){return "/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);}
struct stat info(int fd){struct stat status{};need(fstat(fd,&status)==0,"Cannot inspect controller update file");return status;}
bool fat(int fd){struct statfs status{};return fstatfs(fd,&status)==0&&(status.f_type==0x2011bab0||status.f_type==0x4d44);}
Fd directory(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos,"Invalid controller update directory");
    Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect controller update directory");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(next.value>=0,"Controller update directory is missing or linked");current=std::move(next);}
    return current;
}
struct Folder {
    fs::path path;
    Fd fd;
    struct stat identity;
    std::string mount;
    Folder(fs::path p,bool recovery=false):path(std::move(p)),fd(directory(path)),identity(info(fd.value)),mount(mountIdentity(fd.value)){
        need(identity.st_uid==geteuid()&&(!(identity.st_mode&0022)||fat(fd.value)),"Unsafe controller update directory ownership");
        if(recovery)need(path.parent_path()=="/tmp"&&path.filename().string().rfind("mpc-usb-session.",0)==0&&(identity.st_mode&0777)==0700,"Invalid controller recovery directory");
    }
    void recheck(){auto current=directory(path);const auto now=info(current.value);need(now.st_dev==identity.st_dev&&now.st_ino==identity.st_ino&&mountIdentity(current.value)==mount,"Controller update storage was removed or replaced");}
    void sync(){need(fsync(fd.value)==0,"Cannot sync controller update directory");recheck();}
    bool present(const char* name){struct stat status{};if(fstatat(fd.value,name,&status,AT_SYMLINK_NOFOLLOW)==0)return true;need(errno==ENOENT,"Cannot inspect controller update state");return false;}
    Fd file(const char* name){Fd result(openat(fd.value,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));need(result.value>=0,"Missing or linked controller update file");const auto status=info(result.value);need(S_ISREG(status.st_mode)&&status.st_nlink==1&&status.st_uid==geteuid()&&(!(status.st_mode&0022)||fat(fd.value)),"Unsafe controller update file");return result;}
    std::string read(const char* name,size_t limit){auto input=file(name);const auto status=info(input.value);need(status.st_size>=0&&uint64_t(status.st_size)<=limit,"Oversized controller update state");std::string value;std::array<char,4096> bytes{};for(;;){const auto count=::read(input.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0&&value.size()+size_t(count)<=limit,"Cannot read controller update state");if(!count)break;value.append(bytes.data(),size_t(count));}need(value.size()==uint64_t(status.st_size),"Controller update state changed while reading");return value;}
};
void helperPath(const fs::path& path){
    need(geteuid()==0&&pmu_helper_directory_valid(path.c_str()),"Controller update is outside the installed helper");
#if !defined(__aarch64__)
    need(path==PMU_HELPER_DIRECTORY,"Unsupported Gen 1 controller update location");
#endif
}
std::string controllerName(){return std::string(managerArchitecture())=="aarch64"?"manager-entry-aarch64.so":"manager-entry.so";}
void copy(int input,Folder& target,const char* name,const std::string& expected,mode_t mode){
    target.recheck();Fd output(openat(target.fd.value,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Controller update staging file already exists");
    try{
        const auto source=info(input);need(source.st_size>=52&&source.st_size<=64*1024*1024,"Invalid controller update image size");
        std::array<char,65536> bytes{};off_t offset=0;
        for(;;){const auto count=pread(input,bytes.data(),bytes.size(),offset);if(count<0&&errno==EINTR)continue;need(count>=0,"Cannot read controller update image");if(!count)break;offset+=count;need(offset<=source.st_size,"Controller update image grew during copy");size_t at=0;while(at<size_t(count)){const auto written=write(output.value,bytes.data()+at,size_t(count)-at);if(written<0&&errno==EINTR)continue;need(written>0,"Cannot write controller update image");at+=size_t(written);}}
        need(offset==source.st_size&&fchmod(output.value,mode)==0&&fsync(output.value)==0,"Cannot finish controller update image");auto result=target.file(name);need(fileSha256(descriptor(result.value))==expected,"Controller update copy checksum mismatch");target.sync();
    }catch(...){struct stat named{};const auto created=info(output.value);if(fstatat(target.fd.value,name,&named,AT_SYMLINK_NOFOLLOW)==0&&named.st_dev==created.st_dev&&named.st_ino==created.st_ino)unlinkat(target.fd.value,name,0);throw;}
}
void save(Folder& folder,const Json& value){const auto text=value.dump()+'\n';Fd output(openat(folder.fd.value,record,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Controller update already staged");size_t at=0;while(at<text.size()){const auto count=write(output.value,text.data()+at,text.size()-at);if(count<0&&errno==EINTR)continue;need(count>0,"Cannot write controller update state");at+=size_t(count);}need(fsync(output.value)==0,"Cannot sync controller update state");folder.sync();}
std::optional<Json> state(Folder& recovery){
    if(!recovery.present(record))return {};
    const auto value=parseMetadata(recovery.read(record,4096));need(value.is_object()&&value.size()==6&&value.value("protocol",0)==1&&value.at("architecture")==std::string(managerArchitecture()),"Invalid controller update state");
    helperPath(value.at("helper").get<std::string>());for(const char* key:{"previous","replacement","recovery"})need(hex(value.at(key).get<std::string>()),"Invalid controller update checksum");return value;
}
void publish(Folder& recovery,const Json& value,bool restoring,const VersionStore::Checkpoint& checkpoint={}){
    Folder helper(value.at("helper").get<std::string>());const auto name=controllerName();const auto wanted=value.at(restoring?"previous":"replacement").get<std::string>();
    auto current=helper.file(name.c_str());const auto before=fileSha256(descriptor(current.value));
    if(before==wanted){if(restoring&&helper.present(temporary)){auto staged=helper.file(temporary);const auto hash=fileSha256(descriptor(staged.value));need(hash==value.at("replacement")||hash==value.at("previous"),"Unrelated controller staging file was preserved");need(unlinkat(helper.fd.value,temporary,0)==0,"Cannot clear cancelled controller staging");helper.sync();}return;}
    need(before==value.at(restoring?"replacement":"previous"),"Controller was modified outside this update; preserved");
    const auto source=restoring?"controller-old.so":"controller-new.so";auto image=recovery.file(source);need(fileSha256(descriptor(image.value))==wanted,"Controller recovery image changed");
    if(helper.present(temporary)){auto old=helper.file(temporary);need(fileSha256(descriptor(old.value))==wanted,"Unrelated controller staging file was preserved");need(unlinkat(helper.fd.value,temporary,0)==0,"Cannot clear interrupted controller staging");helper.sync();}
    copy(image.value,helper,temporary,wanted,0755);
    if(checkpoint)checkpoint("controller-copy-durable");
    helper.recheck();
    auto named=helper.file(name.c_str());const auto first=info(current.value),now=info(named.value);need(first.st_dev==now.st_dev&&first.st_ino==now.st_ino&&fileSha256(descriptor(named.value))==before,"Controller changed before atomic replacement");
    need(renameat(helper.fd.value,temporary,helper.fd.value,name.c_str())==0,"Cannot publish controller update");helper.sync();
    if(checkpoint)checkpoint("controller-replaced");
}
}
bool stageControllerUpdate(const UpdateBundle& offered,const fs::path& helper,const fs::path& recoveryPath,const std::string& publisher,const VersionStore::Checkpoint& checkpoint){
    if(helper.empty()||!offered.spec.entrypoints.count("controller_v1_"+std::string(managerArchitecture())))return false;
    helperPath(helper);Folder installed(helper),recovery(recoveryPath,true);
    const auto bundle=verifyStagedUpdate(offered.directory,std::string(managerArchitecture()),offered.spec.version,publisher);
    need(bundle.spec.entrypoints.count("controller_policy"),"Controller update has no compatibility policy");
    const auto policy=parseMetadata(readUpdateFile(bundle,bundle.spec.entrypoints.at("controller_policy"),16*1024));need(policy.is_object()&&policy.size()==3&&policy.value("schema",0)==1&&policy.value("abi",0)==1,"Unsupported controller update policy");
    const auto role=bundle.spec.entrypoints.at("controller_v1_"+std::string(managerArchitecture())),replacement=bundle.spec.files.at(role);const auto old=installed.file(controllerName().c_str());const auto previous=fileSha256(descriptor(old.value));
    if(previous==replacement)return false;
    const auto& allowed=policy.at("previous").at(std::string(managerArchitecture()));need(allowed.is_array()&&!allowed.empty()&&allowed.size()<=128,"Invalid previous-controller policy");bool known=false;for(const auto& item:allowed){need(item.is_string()&&hex(item.get<std::string>()),"Invalid approved controller checksum");known=known||item==previous;}need(known,"Installed startup loader is not an approved previous release; no files changed");
    need(info(old.value).st_size<=2*1024*1024,"Previous controller exceeds its size limit");
    const NativeImageSpec candidate{bundle.directory/role,replacement,std::string(managerArchitecture())};validateNativeImage(candidate);need(fs::file_size(candidate.executable)<=2*1024*1024,"New controller exceeds its size limit");
    auto next=pinNativeImage(candidate,recoveryPath);copy(next.descriptor(),recovery,"controller-new.so",replacement,0500);copy(old.value,recovery,"controller-old.so",previous,0500);
    auto guard=pinNativeImage(bundle.worker,recoveryPath);copy(guard.descriptor(),recovery,"controller-recovery",bundle.worker.sha256,0500);
    save(recovery,{{"protocol",1},{"architecture",std::string(managerArchitecture())},{"helper",helper.string()},{"previous",previous},{"replacement",replacement},{"recovery",bundle.worker.sha256}});
    if(checkpoint)checkpoint("controller-staged");
    return true;
}
void applyControllerUpdate(const fs::path& path,const VersionStore::Checkpoint& checkpoint){Folder recovery(path,true);const auto value=state(recovery);if(value)publish(recovery,*value,false,checkpoint);}
void rollbackControllerUpdate(const fs::path& path){Folder recovery(path,true);const auto value=state(recovery);if(value)publish(recovery,*value,true);}
std::optional<NativeImageSpec> controllerRecovery(const fs::path& path){Folder recovery(path,true);const auto value=state(recovery);if(!value)return {};NativeImageSpec image{path/"controller-recovery",value->at("recovery").get<std::string>(),std::string(managerArchitecture())};validateNativeImage(image);return image;}
int recoverControllerHandoff(const fs::path& capsule){
    const auto handoff=readSessionHandoff(capsule);Folder recovery(handoff.rollbackBase,true);const auto value=state(recovery);need(value.has_value()&&value->at("helper")==handoff.helper.string(),"Controller recovery belongs to another handover");need(runningImageSha256()==value->at("recovery"),"Another program cannot recover this controller update");
    std::string controllerFailure;
    if(recovery.present("handoff.complete")){
        need(recovery.read("handoff.complete",65)==handoff.generation+'\n',"Controller completion belongs to another generation");Folder helper(handoff.helper);auto current=helper.file(controllerName().c_str());need(fileSha256(descriptor(current.value))==value->at("replacement"),"Updated controller changed before confirmation");
    }else try{rollbackControllerUpdate(handoff.rollbackBase);}catch(const std::exception& error){controllerFailure=error.what();}
    runRecoveryWorker(handoff.rollbackWorker,handoff.rollbackBase,capsule);
    need(controllerFailure.empty(),"MPC recovery ran, but controller recovery needs attention: "+controllerFailure);return 0;
}
}
