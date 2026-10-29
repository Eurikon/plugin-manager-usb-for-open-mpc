#include "checksum.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace pmu {
namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {
    int value=-1;
    ~Fd(){if(value>=0)close(value);}
};
struct Child {
    pid_t pid=-1;
    bool reaped=false;
    ~Child(){if(pid>0&&!reaped){kill(-pid,SIGKILL);int status;while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}
};
}
std::string fileSha256(const std::filesystem::path& path,std::chrono::milliseconds timeout){
    need(timeout.count()>0&&timeout<=std::chrono::minutes(3),"Invalid checksum deadline");
    need(!path.empty()&&path.string().find('\0')==std::string::npos,"Invalid checksum path");
    int pipes[2];need(pipe2(pipes,O_CLOEXEC|O_NONBLOCK)==0,"Cannot create checksum pipe");
    Fd input{pipes[0]},output{pipes[1]};
    for(auto* fd:{&input,&output})if(fd->value<3){const int replacement=fcntl(fd->value,F_DUPFD_CLOEXEC,3);need(replacement>=0,"Cannot retain checksum pipe");close(fd->value);fd->value=replacement;}
    posix_spawn_file_actions_t actions;
    need(posix_spawn_file_actions_init(&actions)==0,"Cannot initialize checksum process actions");
    posix_spawnattr_t attributes;
    if(posix_spawnattr_init(&attributes)!=0){posix_spawn_file_actions_destroy(&actions);throw std::runtime_error("Cannot initialize checksum process attributes");}
    sigset_t empty,defaults;sigemptyset(&empty);sigfillset(&defaults);sigdelset(&defaults,SIGKILL);sigdelset(&defaults,SIGSTOP);
    int error=posix_spawn_file_actions_adddup2(&actions,output.value,STDOUT_FILENO);
    error|=posix_spawn_file_actions_addclose(&actions,input.value);
    error|=posix_spawn_file_actions_addclose(&actions,output.value);
    error|=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    error|=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    error|=posix_spawnattr_setsigmask(&attributes,&empty);
    error|=posix_spawnattr_setsigdefault(&attributes,&defaults);
    error|=posix_spawnattr_setpgroup(&attributes,0);
    error|=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETPGROUP);
    const char* executable=access("/usr/bin/sha256sum",X_OK)==0?"/usr/bin/sha256sum":"/bin/sha256sum";
    char lang[]="LC_ALL=C",search[]="PATH=/usr/bin:/bin";
    char* environment[]={lang,search,nullptr};
    char* arguments[]={const_cast<char*>(executable),const_cast<char*>("--"),const_cast<char*>(path.c_str()),nullptr};
    Child child;
    if(!error)error=posix_spawn(&child.pid,executable,&actions,&attributes,arguments,environment);
    posix_spawnattr_destroy(&attributes);posix_spawn_file_actions_destroy(&actions);
    if(error)child.pid=-1;
    close(output.value);output.value=-1;need(error==0,"Cannot start checksum process");
    const auto deadline=std::chrono::steady_clock::now()+timeout;std::string result;std::array<char,512> buffer{};
    bool eof=false;int status=0;
    while(!eof||!child.reaped){
        if(!eof)for(;;){const auto count=read(input.value,buffer.data(),buffer.size());if(count<0&&errno==EINTR)continue;if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;need(count>=0,"Cannot read checksum result");if(count==0){eof=true;break;}need(result.size()+size_t(count)<=4096,"Oversized checksum result");result.append(buffer.data(),size_t(count));}
        if(!child.reaped){const auto found=waitpid(child.pid,&status,WNOHANG);if(found==child.pid)child.reaped=true;else need(found==0||(found<0&&errno==EINTR),"Cannot wait for checksum process");}
        if(eof&&child.reaped)break;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());need(remaining.count()>0,"Checksum process timed out");
        pollfd event{eof?-1:input.value,POLLIN,0};const auto waited=poll(&event,1,int(std::min<int64_t>(remaining.count(),eof?1:50)));need(waited>=0||errno==EINTR,"Checksum wait failed");
    }
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0&&result.size()>64&&result.substr(0,64).find_first_not_of("0123456789abcdef")==std::string::npos&&(result[64]==' '||result[64]=='\t'),"Checksum failed");
    return result.substr(0,64);
}
std::string runningImageSha256(){
    Fd image{open("/proc/self/exe",O_RDONLY|O_CLOEXEC)};
    need(image.value>=0,"Cannot identify the running manager executable");
    return fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(image.value));
}
}
