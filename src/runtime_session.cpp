#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "runtime_session.hpp"
#include "checksum.hpp"
#include "platform.hpp"
#include "startup/storage.h"
#include "metadata.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other)noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;}
};
Fd folder(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos,"Invalid session source path");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect session source");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(next.value>=0,"Session source is missing or linked");current=std::move(next);}return current;
}
bool fat(int fd){struct statfs status{};return fstatfs(fd,&status)==0&&(status.f_type==0x2011bab0||status.f_type==0x4d44);}
void ownedFile(int fd,uint64_t limit){struct stat status{};need(fstat(fd,&status)==0&&S_ISREG(status.st_mode)&&status.st_uid==geteuid()&&(!(status.st_mode&0022)||fat(fd))&&status.st_nlink==1&&status.st_size>=0&&uint64_t(status.st_size)<=limit,"Unsafe or oversized session source file");}
void writeAll(int fd,const char* bytes,size_t length){size_t offset=0;while(offset<length){const auto count=write(fd,bytes+offset,length-offset);if(count<0&&errno==EINTR)continue;need(count>0,"Cannot write session runtime");offset+=size_t(count);}}
void save(int directory,const std::string& name,const std::string& bytes){Fd file(openat(directory,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));need(file.value>=0,"Cannot create session context");writeAll(file.value,bytes.data(),bytes.size());need(fsync(file.value)==0,"Cannot sync session context");}
void copy(const fs::path& source,int destination,const std::string& name,const std::string& hash,bool executable){
    auto parent=folder(source.parent_path());Fd input(openat(parent.value,source.filename().c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));need(input.value>=0,"Cannot open signed runtime file");ownedFile(input.value,64ULL*1024*1024);
    Fd output(openat(destination,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Cannot create RAM runtime file");std::array<char,65536> bytes{};uint64_t total=0;
    for(;;){const auto count=read(input.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0,"Cannot read signed runtime file");if(!count)break;total+=uint64_t(count);need(total<=64ULL*1024*1024,"Runtime file grew during copy");writeAll(output.value,bytes.data(),size_t(count));}
    need(fsync(output.value)==0&&fchmod(output.value,executable?0700:0600)==0,"Cannot finish RAM runtime file");
    need(fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(output.value))==hash,"RAM runtime checksum mismatch");
}
int command(const std::vector<std::string>& arguments,const fs::path& helper,std::chrono::seconds timeout=std::chrono::seconds(20),std::string* output=nullptr){
    Fd capture;posix_spawn_file_actions_t actions;need(posix_spawn_file_actions_init(&actions)==0,"Cannot prepare runtime command descriptors");
    if(output){char pattern[]="/tmp/pmu-command.XXXXXX";capture=Fd(mkostemp(pattern,O_CLOEXEC));if(capture.value<0||unlink(pattern)||posix_spawn_file_actions_adddup2(&actions,capture.value,STDOUT_FILENO)){posix_spawn_file_actions_destroy(&actions);throw std::runtime_error("Cannot capture runtime command output");}}
    std::vector<std::string> environment={"PATH=/usr/sbin:/usr/bin:/sbin:/bin","LANG=C"};if(!helper.empty())environment.push_back("MPC_MANAGER_SD_HELPER="+helper.string());
    std::vector<char*> argv,env;for(const auto& value:arguments)argv.push_back(const_cast<char*>(value.c_str()));argv.push_back(nullptr);for(auto& value:environment)env.push_back(value.data());env.push_back(nullptr);
    posix_spawnattr_t attributes;need(posix_spawnattr_init(&attributes)==0,"Cannot prepare runtime command");sigset_t empty,defaults;sigemptyset(&empty);sigfillset(&defaults);sigdelset(&defaults,SIGKILL);sigdelset(&defaults,SIGSTOP);
    const int error=posix_spawnattr_setsigmask(&attributes,&empty)|posix_spawnattr_setsigdefault(&attributes,&defaults)|posix_spawnattr_setpgroup(&attributes,0)|posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETPGROUP);
    pid_t child=-1;const auto spawned=error?error:posix_spawn(&child,argv[0],&actions,&attributes,argv.data(),env.data());posix_spawnattr_destroy(&attributes);posix_spawn_file_actions_destroy(&actions);need(spawned==0,"Cannot start runtime command");
    const auto deadline=std::chrono::steady_clock::now()+timeout;int status=0;
    while(std::chrono::steady_clock::now()<deadline){if(output){struct stat size{};if(fstat(capture.value,&size)||size.st_size<0||size.st_size>65536)break;}const auto done=waitpid(child,&status,WNOHANG);if(done==child){if(output){need(lseek(capture.value,0,SEEK_SET)==0,"Cannot read runtime command output");std::array<char,4096> bytes{};for(;;){const auto count=read(capture.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0&&output->size()+size_t(count)<=65536,"Oversized runtime command output");if(!count)break;output->append(bytes.data(),size_t(count));}}return WIFEXITED(status)?WEXITSTATUS(status):1;}if(done<0&&errno!=EINTR)break;usleep(20000);}
    kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}throw std::runtime_error("Runtime command timed out");
}
bool ready(int directory){
    Fd file(openat(directory,"ready",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));if(file.value<0){need(errno==ENOENT,"Cannot inspect runtime readiness");return false;}ownedFile(file.value,6);std::array<char,7> bytes{};return read(file.value,bytes.data(),bytes.size())==6&&std::string(bytes.data(),6)=="READY\n";
}
using Roles=std::map<std::string,std::pair<std::string,bool>>;
Roles sessionRoles(const std::string& architecture){return {
    {"manager_"+architecture,{"manager",true}},{"supervisor_"+architecture,{"shortcut",true}},
    {"hook_"+architecture,{"shortcut-hook.so",false}},{"font",{"font.ttf",false}},
    {"launch",{"launch.sh",false}},{"setup",{"setup.sh",false}}};}
void basePath(const fs::path& base){
    const auto name=base.filename().string();need(base.parent_path()=="/tmp"&&name.rfind("mpc-usb-session.",0)==0&&name.size()>16&&name.size()<80&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"Invalid prepared runtime folder");
}
std::string readPrivate(int directory,const std::string& name,size_t maximum){
    Fd input(openat(directory,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(input.value>=0,"Cannot read prepared runtime context");ownedFile(input.value,maximum);struct stat status{};need(fstat(input.value,&status)==0&&(status.st_mode&0777)==0600,"Unsafe prepared runtime context permissions");std::string text;std::array<char,4096> bytes{};
    for(;;){const auto count=read(input.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0&&text.size()+size_t(count)<=maximum,"Invalid prepared runtime context");if(!count)break;text.append(bytes.data(),size_t(count));}return text;
}
struct Prepared {
    nlohmann::json context;
    RuntimeChoice selected;
    Fd directory;
};
Prepared inspect(const fs::path& base,const std::string& publisher,bool retained=false){
    basePath(base);auto directory=folder(base);struct stat status{};need(fstat(directory.value,&status)==0&&status.st_uid==0&&(status.st_mode&0777)==0700,"Unsafe prepared runtime folder");const auto context=parseMetadata(readPrivate(directory.value,"runtime-context.json",8192));need(context.is_object()&&context.value("protocol",0)==1,"Unsupported prepared runtime context");
    SourceRuntime source(context.at("source").get<std::string>(),managerArchitecture(),publisher);const auto selected=retained?source.retained(context.at("generation").get<std::string>(),context.at("source_identity").get<std::string>()):source.choose(false,context.at("source_identity").get<std::string>());need(selected.generation.id==context.at("generation").get<std::string>(),"Prepared runtime version is no longer selected");
    const auto roles=sessionRoles(managerArchitecture());
    for(const auto& role:roles){const auto file=selected.generation.spec.entrypoints.at(role.first);Fd input(openat(directory.value,role.second.first.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(input.value>=0,"Prepared runtime is incomplete");ownedFile(input.value,64ULL*1024*1024);need(fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(input.value))==selected.generation.spec.files.at(file),"Prepared runtime checksum mismatch");}
    const auto native=selected.generation.spec.entrypoints.at("runtime_worker_"+std::string(managerArchitecture())),manager=selected.generation.spec.entrypoints.at("manager_"+std::string(managerArchitecture()));const std::string worker=selected.generation.spec.files.at(native)==selected.generation.spec.files.at(manager)?"manager":"runtime-worker";need(context.at("worker")==worker,"Prepared runtime worker changed");
    Fd input(openat(directory.value,worker.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(input.value>=0,"Prepared runtime worker is missing");ownedFile(input.value,64ULL*1024*1024);need(fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(input.value))==selected.generation.spec.files.at(native),"Prepared runtime worker checksum mismatch");return {context,selected,std::move(directory)};
}
void start(const fs::path& base,const Prepared& prepared,const std::string& publisher){
    const fs::path source=prepared.context.at("source").get<std::string>(),helper=prepared.context.at("helper").get<std::string>();
    if(!helper.empty()){
        need(pmu_helper_directory_valid(helper.c_str()),"Unsupported prepared helper location");
#if !defined(__aarch64__)
        need(helper==PMU_HELPER_DIRECTORY,"Unsupported prepared Gen 1 helper location");
#endif
    }
    struct stat snapshot{};const bool resume=fstatat(prepared.directory.value,"preload.snapshot",&snapshot,AT_SYMLINK_NOFOLLOW)==0;if(!resume)need(errno==ENOENT,"Cannot inspect addin handover snapshot");if(resume)(void)readPrivate(prepared.directory.value,"preload.snapshot",1024*1024);
    need(command({"/bin/sh",(base/"setup.sh").string(),source.string(),base.string(),resume?"--resume-only":"--arm-only"},helper)==0,"Manager supervisor refused preparation");
    SourceRuntime current(source,managerArchitecture(),publisher);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!ready(prepared.directory.value)){need(std::chrono::steady_clock::now()<deadline,"Manager supervisor readiness timed out");current.identity();usleep(25000);}
    need(current.choose(false,prepared.selected.identity).generation.id==prepared.selected.generation.id,"Manager source changed before readiness");
}
}
fs::path prepareRuntimeSession(const RuntimeChoice& requested,const fs::path& source,const fs::path& helper,const std::string& publisher,const UpdateProgress& progress,const std::string& snapshot,RuntimeStart mode,bool retained){
    need(geteuid()==0,"Runtime startup requires the manager service");const std::string architecture=managerArchitecture();SourceRuntime current(source,architecture,publisher);const auto selected=retained?current.retained(requested.generation.id,requested.identity):current.choose(false,requested.identity);need(selected.generation.id==requested.generation.id,"Manager source changed before session preparation");
    if(!helper.empty()){
        need(pmu_helper_directory_valid(helper.c_str()),"Unsupported manager helper location");
#if !defined(__aarch64__)
        need(helper==PMU_HELPER_DIRECTORY,"Unsupported Gen 1 helper location");
#endif
        auto directory=folder(helper);struct stat status{};need(fstat(directory.value,&status)==0&&status.st_uid==0&&(!(status.st_mode&0022)||pmu_helper_exfat(directory.value)),"Unsafe manager helper directory");
    }
    need(snapshot.size()<=1024*1024&&snapshot.find('\0')==std::string::npos,"Invalid addin handover snapshot");
    const auto roles=sessionRoles(architecture);
    uint64_t bytes=0;for(const auto& role:roles){const auto found=selected.generation.spec.entrypoints.find(role.first);need(found!=selected.generation.spec.entrypoints.end(),"Manager version lacks a required session role");bytes+=fs::file_size(selected.generation.directory/found->second);}need(bytes<=128ULL*1024*1024&&fs::space("/tmp").available>=bytes+1024*1024,"Insufficient RAM space for the complete manager session");
    char pattern[]="/tmp/mpc-usb-session.XXXXXX";const char* made=mkdtemp(pattern);need(made!=nullptr,"Cannot stage manager runtime in RAM");const fs::path base=made;bool supervisor=false;
    try{
        auto destination=folder(base);if(progress.message)progress.message("Preparing the verified manager session...");
        for(const auto& role:roles){const auto name=selected.generation.spec.entrypoints.at(role.first);copy(selected.generation.directory/name,destination.value,role.second.first,selected.generation.spec.files.at(name),role.second.second);}
        const auto worker=selected.generation.spec.entrypoints.at("runtime_worker_"+architecture),manager=selected.generation.spec.entrypoints.at("manager_"+architecture);std::string workerName="manager";
        if(selected.generation.spec.files.at(worker)!=selected.generation.spec.files.at(manager)){workerName="runtime-worker";copy(selected.generation.directory/worker,destination.value,workerName,selected.generation.spec.files.at(worker),true);}
        save(destination.value,"runtime-context.json",nlohmann::json({{"protocol",1},{"source",source.string()},{"helper",helper.string()},{"generation",selected.generation.id},{"source_identity",selected.identity},{"worker",workerName},{"cohort",base.string()}}).dump()+'\n');
        if(!snapshot.empty())save(destination.value,"preload.snapshot",snapshot);
        need(fsync(destination.value)==0,"Cannot finish RAM session staging");need((retained?current.retained(selected.generation.id,selected.identity):current.choose(false,selected.identity)).generation.id==selected.generation.id,"Manager source changed during session staging");
        if(mode==RuntimeStart::StageOnly)return base;
        need(!retained,"Retained recovery staging cannot start before restoring its selection");
        supervisor=true;
        start(base,inspect(base,publisher),publisher);return base;
    }catch(...){
        if(!supervisor){std::error_code ignored;fs::remove_all(base,ignored);}
        else try{command({(base/"shortcut").string(),"--cancel-prepared",base.string()},helper,std::chrono::seconds(8));}catch(...){}
        throw;
    }
}
void startPreparedRuntime(const fs::path& base,const std::string& publisher){start(base,inspect(base,publisher),publisher);}
NativeImageSpec preparedRuntimeWorker(const fs::path& base,const std::string& publisher,bool retained){const auto prepared=inspect(base,publisher,retained);const auto worker=prepared.selected.generation.spec.entrypoints.at("runtime_worker_"+std::string(managerArchitecture()));return {base/prepared.context.at("worker").get<std::string>(),prepared.selected.generation.spec.files.at(worker),managerArchitecture()};}
nlohmann::json guiRuntimeContext(const fs::path& work){
    const auto name=work.filename().string();need(work.parent_path()=="/tmp"&&name.rfind("mpc-usb-manager.",0)==0&&name.size()>16&&name.size()<80&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"Invalid GUI runtime folder");auto directory=folder(work);struct stat status{};need(fstat(directory.value,&status)==0&&status.st_uid==0&&(status.st_mode&0777)==0700,"Unsafe GUI runtime folder");const auto context=parseMetadata(readPrivate(directory.value,"runtime-context.json",8192));need(context.is_object()&&context.value("protocol",0)==1,"Unsupported GUI runtime context");return context;
}
int runtimeCommand(const std::vector<std::string>& arguments,const fs::path& helper,std::chrono::seconds timeout){need(!arguments.empty(),"Missing runtime command");return command(arguments,helper,timeout);}
std::pair<int,std::string> runtimeOutput(const std::vector<std::string>& arguments,std::chrono::seconds timeout){need(!arguments.empty(),"Missing runtime query");std::string text;const auto status=command(arguments,{},timeout,&text);return {status,text};}
}
