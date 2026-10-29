#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "session_handoff.hpp"
#include "metadata.hpp"
#include "platform.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {
    int value=-1;explicit Fd(int fd=-1):value(fd){}~Fd(){if(value>=0)close(value);}Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other)noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;}
};
Fd folder(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos,"Invalid update handover path");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect handover root");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(next.value>=0,"Handover path is missing or linked");current=std::move(next);}return current;
}
bool basePath(const fs::path& path,const std::string& prefix){const auto name=path.filename().string();return path.parent_path()=="/tmp"&&name.rfind(prefix,0)==0&&name.size()>prefix.size()&&name.size()<80&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos;}
void privateFolder(int fd){struct stat status{};need(fstat(fd,&status)==0&&S_ISDIR(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0700,"Unsafe private handover folder");}
Fd cacheFolder(const fs::path& cache){
    need(cache.filename()=="cache"&&basePath(cache.parent_path(),"mpc-usb-manager."),"Update did not come from a manager session");auto parent=folder(cache.parent_path());privateFolder(parent.value);auto result=folder(cache);struct stat status{};need(fstat(result.value,&status)==0&&status.st_uid==0&&!(status.st_mode&0022),"Unsafe manager update cache");return result;
}
std::string read(int directory,const std::string& name,size_t limit,bool exact=true){
    Fd input(openat(directory,name.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW));need(input.value>=0,"Cannot read update handover state");struct stat before{},after{};need(fstat(input.value,&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==0&&before.st_nlink==1&&!(before.st_mode&0022)&&(!exact||(before.st_mode&0777)==0600)&&before.st_size>=0&&uint64_t(before.st_size)<=limit,"Unsafe update handover state");std::string text;std::array<char,4096> bytes{};
    for(;;){const auto count=::read(input.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0&&text.size()+size_t(count)<=limit,"Invalid update handover state");if(!count)break;text.append(bytes.data(),size_t(count));}
    need(fstat(input.value,&after)==0&&text.size()==size_t(before.st_size)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"Update handover state changed while reading");return text;
}
void save(int directory,const std::string& name,const std::string& text){
    Fd output(openat(directory,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));need(output.value>=0,"Another update handover is already queued");size_t at=0;
    while(at<text.size()){const auto count=write(output.value,text.data()+at,text.size()-at);if(count<0&&errno==EINTR)continue;need(count>0,"Cannot write update handover state");at+=size_t(count);}need(fsync(output.value)==0&&fsync(directory)==0,"Cannot finish update handover state");
}
std::pair<fs::path,fs::path> running(){auto root=folder("/run");const auto value=read(root.value,"mpc-usb-manager-session",8192);std::istringstream input(value);std::string base,source,extra;need(std::getline(input,base)&&std::getline(input,source)&&!std::getline(input,extra)&&value==base+'\n'+source+'\n',"Invalid running manager record");need(basePath(base,"mpc-usb-session."),"Invalid running manager cohort");return {base,source};}
std::string service(const fs::path& base){
    std::string selected;for(const auto& name:{std::string("acvs"),std::string("inmusic-mpc")}){
        const auto path=fs::path("/run/systemd/system")/(name+".service.d");struct stat status{};if(lstat(path.c_str(),&status)!=0){need(errno==ENOENT,"Cannot inspect manager service");continue;}auto directory=folder(path);if(fstatat(directory.value,"99-mpc-usb-shortcut.conf",&status,AT_SYMLINK_NOFOLLOW)!=0){need(errno==ENOENT,"Cannot inspect manager drop-in");continue;}
        need(read(directory.value,"99-mpc-usb-shortcut.conf",8192,false)=="[Service]\nEnvironmentFile="+(base/"shortcut.env").string()+"\n","Manager service belongs to another cohort");need(selected.empty(),"Ambiguous manager service handover");selected=name;
    }need(!selected.empty(),"No owned MPC manager service is available");return selected;
}
}
struct UpdateSession::Impl {
    fs::path source,cache,oldBase;
    std::string identity,snapshot,serviceName,previousGeneration;
    Fd cacheDirectory,baseDirectory,lock;
    Impl(fs::path path,fs::path session,std::string generation,std::string cookie):source(std::move(path)),cache(std::move(session)),identity(std::move(cookie)),previousGeneration(generation),cacheDirectory(cacheFolder(cache)){
        lock=Fd(openat(cacheDirectory.value,"update-handoff.lock",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
        if(lock.value<0&&errno==EEXIST)lock=Fd(openat(cacheDirectory.value,"update-handoff.lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
        struct stat status{};
        need(lock.value>=0&&fstat(lock.value,&status)==0&&S_ISREG(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0600&&status.st_nlink==1&&flock(lock.value,LOCK_EX|LOCK_NB)==0,"Another manager update is already running");
        need(fstatat(cacheDirectory.value,"manager-update.pending",&status,AT_SYMLINK_NOFOLLOW)!=0&&errno==ENOENT,"Return to MPC to finish the queued manager update");const auto active=running();need(active.second==source,"Another source owns the running manager");oldBase=active.first;baseDirectory=folder(oldBase);privateFolder(baseDirectory.value);const auto context=parseMetadata(read(baseDirectory.value,"runtime-context.json",8192));need(context.value("protocol",0)==1&&context.at("source")==source.string()&&context.at("generation")==generation&&context.at("source_identity")==identity,"The running manager cohort changed before updating");snapshot=read(baseDirectory.value,"shortcut.env",1024*1024);serviceName=service(oldBase);
    }
};
UpdateSession::UpdateSession(fs::path source,fs::path cache,std::string generation,std::string identity):impl(std::make_unique<Impl>(std::move(source),std::move(cache),std::move(generation),std::move(identity))){}
UpdateSession::~UpdateSession()=default;
std::string UpdateSession::preloadSnapshot()const{return impl->snapshot;}
std::string UpdateSession::mpcService()const{return impl->serviceName;}
fs::path UpdateSession::publish(const fs::path& base,const fs::path& helper,const std::string& generation,const fs::path& rollback,const NativeImageSpec& rollbackWorker,const std::string& helperGeneration,const std::string& previousHelper,bool open){
    need(basePath(base,"mpc-usb-session.")&&base!=impl->oldBase&&generation.size()==64&&generation.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid prepared update handover");auto directory=folder(base);privateFolder(directory.value);const auto active=running();need(active.first==impl->oldBase&&active.second==impl->source&&service(impl->oldBase)==impl->serviceName,"Running manager changed during update preparation");
    need(basePath(rollback,"mpc-usb-session.")&&rollback!=base&&rollback!=impl->oldBase&&rollbackWorker.executable.parent_path()==rollback&&rollbackWorker.architecture==managerArchitecture()&&rollbackWorker.sha256.size()==64&&rollbackWorker.sha256.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid retained recovery cohort");auto recovery=folder(rollback);privateFolder(recovery.value);
    const auto capsule=rollback/"update-handoff.json";save(recovery.value,capsule.filename().string(),Json({{"protocol",1},{"source",impl->source.string()},{"helper",helper.string()},{"old_base",impl->oldBase.string()},{"new_base",base.string()},{"rollback_base",rollback.string()},{"generation",generation},{"previous_generation",impl->previousGeneration},{"helper_generation",helperGeneration},{"previous_helper_generation",previousHelper},{"source_identity",impl->identity},{"service",impl->serviceName},{"rollback_worker",rollbackWorker.executable.string()},{"rollback_worker_sha256",rollbackWorker.sha256},{"open_after_handoff",open}}).dump()+'\n');
    const auto temporary=".manager-update.pending."+std::to_string(getpid());save(impl->cacheDirectory.value,temporary,capsule.string()+'\n');
    if(syscall(SYS_renameat2,impl->cacheDirectory.value,temporary.c_str(),impl->cacheDirectory.value,"manager-update.pending",RENAME_NOREPLACE)!=0){unlinkat(impl->cacheDirectory.value,temporary.c_str(),0);throw std::runtime_error("Cannot publish manager handover without replacing existing state");}need(fsync(impl->cacheDirectory.value)==0,"Cannot finish manager handover publication");return capsule;
}
SessionHandoff readSessionHandoff(const fs::path& path){
    need(path.filename()=="update-handoff.json"&&basePath(path.parent_path(),"mpc-usb-session."),"Invalid update handover capsule");auto directory=folder(path.parent_path());privateFolder(directory.value);const auto value=parseMetadata(read(directory.value,path.filename().string(),8192));need(value.is_object()&&value.size()==15&&value.value("protocol",0)==1,"Unsupported update handover capsule");SessionHandoff result{value.at("source").get<std::string>(),value.at("helper").get<std::string>(),value.at("old_base").get<std::string>(),value.at("new_base").get<std::string>(),value.at("rollback_base").get<std::string>(),value.at("generation").get<std::string>(),value.at("previous_generation").get<std::string>(),value.at("helper_generation").get<std::string>(),value.at("previous_helper_generation").get<std::string>(),value.at("source_identity").get<std::string>(),value.at("service").get<std::string>(),{value.at("rollback_worker").get<std::string>(),value.at("rollback_worker_sha256").get<std::string>(),managerArchitecture()},value.at("open_after_handoff").get<bool>()};
    auto normalized=[](const fs::path& value){const auto text=value.string();return value.is_absolute()&&value.lexically_normal()==value&&text.find('\0')==std::string::npos&&text.find_first_of("\r\n")==std::string::npos;};
    auto digest=[](const std::string& text){return text.size()==64&&text.find_first_not_of("0123456789abcdef")==std::string::npos;};need(normalized(result.source)&&(result.helper.empty()||normalized(result.helper))&&normalized(result.rollbackWorker.executable)&&result.rollbackBase==path.parent_path()&&basePath(result.oldBase,"mpc-usb-session.")&&basePath(result.newBase,"mpc-usb-session.")&&result.oldBase!=result.newBase&&result.rollbackBase!=result.oldBase&&result.rollbackBase!=result.newBase&&(result.service=="acvs"||result.service=="inmusic-mpc")&&digest(result.generation)&&digest(result.previousGeneration)&&(result.helperGeneration.empty()||digest(result.helperGeneration))&&(result.previousHelperGeneration.empty()||digest(result.previousHelperGeneration))&&result.rollbackWorker.executable.parent_path()==result.rollbackBase&&digest(result.rollbackWorker.sha256)&&!result.sourceIdentity.empty()&&result.sourceIdentity.size()<=128&&result.sourceIdentity.find_first_not_of("0123456789:")==std::string::npos,"Invalid update handover identity");return result;
}
fs::path queuedSessionHandoff(const fs::path& cache){
    auto directory=cacheFolder(cache);struct stat status{};if(fstatat(directory.value,"manager-update.pending",&status,AT_SYMLINK_NOFOLLOW)!=0){need(errno==ENOENT,"Cannot inspect queued update handover");return {};}
    auto text=read(directory.value,"manager-update.pending",256);need(!text.empty()&&text.back()=='\n'&&text.find_first_of("\r\n")==text.size()-1,"Invalid queued update handover");text.pop_back();const fs::path path=text;need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos&&path.filename()=="update-handoff.json"&&basePath(path.parent_path(),"mpc-usb-session."),"Unsafe queued update handover path");return path;
}
}
