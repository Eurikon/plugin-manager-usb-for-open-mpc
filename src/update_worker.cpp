#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "update_worker.hpp"
#include "checksum.hpp"
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr size_t requestLimit=256*1024,responseLimit=1024*1024;
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
bool hex(const std::string& text,size_t size){return text.size()==size&&text.find_first_not_of("0123456789abcdef")==std::string::npos;}
Fd directory(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path,"Unsafe update-worker directory");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect update-worker directory");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));need(next.value>=0,"Update-worker path is missing or contains a link");current=std::move(next);}return current;
}
struct stat regular(int fd,bool anonymous=false){
    struct stat value{};need(fstat(fd,&value)==0&&S_ISREG(value.st_mode)&&value.st_uid==geteuid()&&value.st_nlink==(anonymous?0:1)&&!(value.st_mode&0022),"Unsafe update-worker file");return value;
}
std::string nonce(){
    Fd input(open("/dev/urandom",O_RDONLY|O_CLOEXEC));need(input.value>=0,"Cannot identify update request");std::array<unsigned char,16> data{};size_t at=0;
    while(at<data.size()){const auto n=read(input.value,data.data()+at,data.size()-at);if(n<0&&errno==EINTR)continue;need(n>0,"Cannot identify update request");at+=size_t(n);}
    std::string result;const char* digits="0123456789abcdef";for(auto byte:data){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
Fd anonymous(int root){
    const auto name=".update-ipc-"+nonce();Fd result(openat(root,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(result.value>=0,"Cannot create private update IPC");
    need(unlinkat(root,name.c_str(),0)==0,"Cannot unlink private update IPC");regular(result.value,true);return result;
}
std::string descriptor(int fd){return "/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);}
Fd readonly(int fd){Fd result(open(descriptor(fd).c_str(),O_RDONLY|O_CLOEXEC));need(result.value>=0,"Cannot pin private update input");regular(result.value,true);return result;}
void writeAll(int fd,const std::string& text){size_t at=0;while(at<text.size()){const auto n=write(fd,text.data()+at,text.size()-at);if(n<0&&errno==EINTR)continue;need(n>0,"Cannot write update-worker IPC");at+=size_t(n);}}
std::string readAll(int fd,size_t limit){
    const auto value=regular(fd,true);need(value.st_size>=0&&uint64_t(value.st_size)<=limit,"Oversized update-worker IPC");std::string text(size_t(value.st_size),'\0');size_t at=0;
    while(at<text.size()){const auto n=pread(fd,text.data()+at,text.size()-at,off_t(at));if(n<0&&errno==EINTR)continue;need(n>0,"Cannot read update-worker IPC");at+=size_t(n);}return text;
}
std::string readResponse(int fd,size_t& offset){
    const auto value=regular(fd,true);need(value.st_size>=0&&uint64_t(value.st_size)<=responseLimit&&uint64_t(value.st_size)>=offset,"Invalid update response size");
    std::string text(size_t(value.st_size)-offset,'\0');size_t at=0;while(at<text.size()){const auto n=pread(fd,text.data()+at,text.size()-at,off_t(offset+at));if(n<0&&errno==EINTR)continue;need(n>0,"Cannot read update response");at+=size_t(n);}offset+=text.size();return text;
}
void elf(int fd,const std::string& architecture){
    std::array<unsigned char,64> header{};const auto count=pread(fd,header.data(),header.size(),0);const bool wide=architecture=="aarch64";
    auto word=[&](size_t at){return unsigned(header[at])|(unsigned(header[at+1])<<8);};
    need((architecture=="armv7"||wide)&&count>=ssize_t(wide?64:52)&&std::memcmp(header.data(),"\177ELF",4)==0&&header[4]==(wide?2:1)&&header[5]==1&&header[6]==1&&(word(16)==2||word(16)==3)&&word(18)==(wide?183:40)&&word(wide?52:40)==(wide?64:52),"Update worker has the wrong native architecture");
}
Fd executable(const IncomingWorker& worker,int scratch){
    need(hex(worker.sha256,64),"Invalid incoming worker checksum");auto parent=directory(worker.executable.parent_path());Fd source(openat(parent.value,worker.executable.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(source.value>=0,"Cannot open incoming update worker");
    const auto before=regular(source.value);need(before.st_size>=52&&uint64_t(before.st_size)<=64*1024*1024,"Invalid incoming worker size");elf(source.value,worker.architecture);need(fileSha256(descriptor(source.value))==worker.sha256,"Incoming worker checksum mismatch");
    auto copy=anonymous(scratch);std::array<char,65536> buffer{};uint64_t bytes=0;
    for(;;){const auto n=read(source.value,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;need(n>=0,"Cannot copy incoming update worker");if(!n)break;bytes+=uint64_t(n);need(bytes<=uint64_t(before.st_size),"Incoming worker changed while copying");writeAll(copy.value,std::string(buffer.data(),size_t(n)));}
    need(bytes==uint64_t(before.st_size)&&fchmod(copy.value,0500)==0&&fsync(copy.value)==0,"Cannot finish incoming worker copy");need(fileSha256(descriptor(copy.value))==worker.sha256,"Incoming worker RAM copy differs");return readonly(copy.value);
}
bool closeOthers(){
    const int folder=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(folder<0)return false;alignas(8)std::array<unsigned char,8192> buffer{};
    for(;;){const auto size=syscall(SYS_getdents64,folder,buffer.data(),buffer.size());if(size<0&&errno==EINTR)continue;if(size<0){close(folder);return false;}if(!size)break;
        for(size_t at=0;at<size_t(size);){if(size_t(size)-at<20){close(folder);return false;}unsigned short length;std::memcpy(&length,buffer.data()+at+16,sizeof length);if(length<20||at+length>size_t(size)){close(folder);return false;}
            int fd=0;bool digits=true,ended=false;for(size_t i=at+19;i<at+length;++i){const auto c=buffer[i];if(!c){ended=true;break;}if(c<'0'||c>'9'||fd>100000000){digits=false;break;}fd=fd*10+c-'0';}if(digits&&ended&&fd>=6&&fd!=folder)close(fd);at+=length;
        }
    }close(folder);return true;
}
struct Child {
    pid_t pid=-1;
    bool reaped=false;
    ~Child(){if(pid>0&&!reaped){kill(-pid,SIGKILL);int status;while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}
};
Fd aboveReserved(int fd){Fd copy(fcntl(fd,F_DUPFD_CLOEXEC,6));need(copy.value>=6,"Cannot retain update-worker descriptor");return copy;}
Json exchange(int program,int scratch,const Json& task,const UpdateProgress& progress,std::chrono::milliseconds timeout){
    const auto requestId=nonce();const Json request={{"protocol",1},{"request_id",requestId},{"task",task}};const auto requestText=request.dump()+'\n';need(requestText.size()<=requestLimit,"Oversized update request");
    auto writable=anonymous(scratch);writeAll(writable.value,requestText);need(fsync(writable.value)==0,"Cannot finish update request");auto requestFile=readonly(writable.value);writable=Fd{};auto response=anonymous(scratch);
    auto childRequest=aboveReserved(requestFile.value),childResponse=aboveReserved(response.value),childProgram=aboveReserved(program);const auto parent=getpid();
    Child child;child.pid=fork();need(child.pid>=0,"Cannot start incoming update worker");
    if(!child.pid){
        if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(126);
        sigset_t empty;sigemptyset(&empty);if(sigprocmask(SIG_SETMASK,&empty,nullptr))_exit(126);
        for(int signal:{SIGPIPE,SIGCHLD,SIGINT,SIGTERM,SIGHUP}){struct sigaction action{};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);if(sigaction(signal,&action,nullptr))_exit(126);}
        if(dup2(childRequest.value,3)<0||dup2(childResponse.value,4)<0||dup2(childProgram.value,5)<0||!closeOthers())_exit(126);
        for(int fd=0;fd<2;++fd){close(fd);if(open("/dev/null",fd==0?O_RDONLY:O_WRONLY)!=fd)_exit(126);}
        char name[]="pmu-update-worker",mode[]="--pmu-update-v1",path[]="PATH=/usr/sbin:/usr/bin:/sbin:/bin",lang[]="LC_ALL=C";char* args[]={name,mode,nullptr};char* environment[]={path,lang,nullptr};
        execve("/proc/self/fd/5",args,environment);_exit(127);
    }
    const auto deadline=std::chrono::steady_clock::now()+timeout;size_t offset=0;std::string partial;std::optional<Json> final;int status=0;
    for(;;){
        partial+=readResponse(response.value,offset);
        for(size_t end;(end=partial.find('\n'))!=std::string::npos;){const auto line=partial.substr(0,end);partial.erase(0,end+1);need(line.size()<=128*1024,"Oversized update response event");const auto event=Json::parse(line);need(event.is_object()&&event.value("protocol",0)==1&&event.value("request_id",std::string())==requestId&&!final,"Invalid update response envelope");const auto kind=event.at("kind").get<std::string>();
            if(kind=="result")final=event;
            else if(kind=="progress"){
                if(event.contains("message")){const auto message=event.at("message").get<std::string>();need(message.size()<=1024,"Oversized update progress message");if(progress.message)progress.message(message);}
                if(event.contains("fraction")){const auto value=event.at("fraction").get<float>();need(std::isfinite(value)&&value>=-1&&value<=1,"Invalid update progress fraction");if(progress.fraction)progress.fraction(value);}
            }
        }
        if(!child.reaped){const auto found=waitpid(child.pid,&status,WNOHANG);if(found==child.pid)child.reaped=true;else need(found==0||(found<0&&errno==EINTR),"Cannot wait for incoming update worker");}
        if(child.reaped){const auto size=regular(response.value,true).st_size;need(size>=0&&uint64_t(size)>=offset&&uint64_t(size)<=responseLimit,"Invalid final update response size");if(uint64_t(size)==offset)break;continue;}
        if(!child.reaped){need(std::chrono::steady_clock::now()<deadline,"Incoming update worker timed out");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    }
    need(partial.empty()&&final.has_value(),"Update worker ended without a complete result; recovery may be needed");
    if(!final->value("ok",false))throw std::runtime_error(final->value("error",std::string("Incoming update worker failed")));
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0&&final->contains("result")&&final->at("result").is_object(),"Incoming update worker did not finish cleanly");return final->at("result");
}
}

IncomingWorker generationWorker(const Generation& generation,const std::string& architecture){
    need((architecture=="armv7"||architecture=="aarch64")&&(generation.spec.architecture=="universal"||generation.spec.architecture==architecture),"Release does not support this architecture");
    const auto role="update_worker_"+architecture;const auto found=generation.spec.entrypoints.find(role);need(found!=generation.spec.entrypoints.end(),"Release has no update worker for this architecture");const auto file=generation.spec.files.find(found->second);need(file!=generation.spec.files.end(),"Update worker is not covered by the generation inventory");return {generation.directory/found->second,file->second,architecture};
}
Json runIncomingWorker(const IncomingWorker& worker,const fs::path& session,const Json& task,const UpdateProgress& progress,std::chrono::milliseconds timeout){
    need(timeout.count()>0&&timeout<=std::chrono::minutes(15),"Invalid update-worker deadline");need(task.is_object(),"Invalid update-worker task");auto scratch=directory(session);struct stat value{};need(fstat(scratch.value,&value)==0&&value.st_uid==geteuid()&&!(value.st_mode&0022),"Unsafe update-worker scratch directory");auto program=executable(worker,scratch.value);
    const auto hello=exchange(program.value,scratch.value,{{"operation","hello"}},{},timeout);need(hello.value("product",std::string())=="plugin-manager-usb"&&hello.value("protocol",0)==1,"Incoming release does not support the stable update protocol");return exchange(program.value,scratch.value,task,progress,timeout);
}
int serveUpdateWorker(const UpdateHandler& handler){
    std::string requestId;size_t written=0;bool reported=false;
    auto event=[&](Json message){message["protocol"]=1;message["request_id"]=requestId;const auto line=message.dump()+'\n';need(line.size()<=128*1024&&written+line.size()<=responseLimit,"Oversized update-worker response");writeAll(4,line);written+=line.size();};
    try{
        regular(3,true);need((fcntl(3,F_GETFL)&O_ACCMODE)==O_RDONLY,"Update request must be read-only");const auto response=regular(4,true);need(response.st_size==0&&(fcntl(4,F_GETFL)&O_ACCMODE)!=O_RDONLY,"Update response must be a fresh writable channel");regular(5,true);close(5);need(fcntl(3,F_SETFD,FD_CLOEXEC)==0&&fcntl(4,F_SETFD,FD_CLOEXEC)==0,"Cannot protect update IPC descriptors");
        const auto request=Json::parse(readAll(3,requestLimit));need(request.is_object()&&request.value("protocol",0)==1&&request.contains("task")&&request.at("task").is_object(),"Unsupported update-worker request");requestId=request.at("request_id").get<std::string>();need(hex(requestId,32),"Invalid update request identifier");const auto task=request.at("task");Json result;
        if(task.value("operation",std::string())=="hello")result={{"product","plugin-manager-usb"},{"protocol",1}};
        else{
            need(task.value("operation",std::string())=="apply"&&bool(handler),"Unsupported update-worker operation");
            auto last=std::chrono::steady_clock::time_point{};
            UpdateProgress progress{[&](const std::string& message){if(written<256*1024){need(message.size()<=1024,"Oversized update progress message");event({{"kind","progress"},{"message",message}});}},[&](float fraction){need(std::isfinite(fraction)&&fraction>=-1&&fraction<=1,"Invalid update progress fraction");const auto now=std::chrono::steady_clock::now();if(written<256*1024&&(fraction==1||now-last>=std::chrono::milliseconds(30))){event({{"kind","progress"},{"fraction",fraction}});last=now;}}};
            result=handler(task,progress);need(result.is_object(),"Invalid incoming update result");
        }
        event({{"kind","result"},{"ok",true},{"result",result}});reported=true;return 0;
    }catch(const std::exception& error){if(!requestId.empty()&&!reported)try{event({{"kind","result"},{"ok",false},{"error",std::string(error.what()).substr(0,2048)}});}catch(...){}return 1;}
}
}
