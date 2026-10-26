#include "shortcut_state.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <climits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

namespace fs=std::filesystem;
static void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message+": "+strerror(errno));}
static uint64_t nowMs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
static int command(const std::vector<std::string>& arguments){
    pid_t pid=fork();require(pid>=0,"fork");
    if(!pid){sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,nullptr);std::vector<char*> argv;for(const auto& argument:arguments)argv.push_back(const_cast<char*>(argument.c_str()));argv.push_back(nullptr);execvp(argv[0],argv.data());_exit(127);}
    int status;while(waitpid(pid,&status,0)<0)if(errno!=EINTR)throw std::runtime_error("waitpid failed");
    return WIFEXITED(status)?WEXITSTATUS(status):1;
}
static std::string serviceProperty(const std::string& service,const char* property,size_t limit=65536){
    int pipes[2];require(pipe2(pipes,O_CLOEXEC)==0,"Create service query pipe");
    pid_t pid=fork();if(pid<0){close(pipes[0]);close(pipes[1]);throw std::runtime_error("Start service query failed");}
    if(!pid){sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,nullptr);close(pipes[0]);if(dup2(pipes[1],STDOUT_FILENO)<0)_exit(127);close(pipes[1]);execlp("systemctl","systemctl","show",service.c_str(),"-p",property,"--value",nullptr);_exit(127);}
    close(pipes[1]);std::string result;char buffer[64];bool valid=true;ssize_t size;
    while((size=read(pipes[0],buffer,sizeof buffer))!=0){if(size<0){if(errno==EINTR)continue;valid=false;break;}if(result.size()+size>limit){valid=false;break;}result.append(buffer,size);}
    close(pipes[0]);if(!valid)kill(pid,SIGKILL);int status=0;while(waitpid(pid,&status,0)<0){if(errno==EINTR)continue;throw std::runtime_error("Wait for service query failed");}
    if(!valid||!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("Cannot read MPC service property");
    while(!result.empty()&&(result.back()=='\n'||result.back()=='\r'))result.pop_back();
    return result;
}
static std::string mainPid(const std::string& service){
    const std::string result=serviceProperty(service,"MainPID",64);
    if(result.empty()||result.size()>10||result.find_first_not_of("0123456789")!=std::string::npos||result[0]=='0'||std::stoull(result)>INT_MAX)throw std::runtime_error("MPC service must be running before opening the manager");
    return result;
}
static std::vector<std::string> propertyWords(const std::string& value){
    std::vector<std::string> result;std::string word;char quote=0;bool escaped=false;
    for(char byte:value){if(escaped){word+=byte;escaped=false;}else if(byte=='\\'&&quote!='\'')escaped=true;else if(byte==quote)quote=0;else if(!quote&&(byte=='\''||byte=='\"'))quote=byte;else if(!quote&&(byte==' '||byte=='\t'||byte=='\n')){if(!word.empty()){result.push_back(word);word.clear();}}else word+=byte;}
    if(escaped||quote)throw std::runtime_error("Invalid MPC service property quoting");
    if(!word.empty())result.push_back(word);
    return result;
}
static bool ownHook(const std::string& value){
    const fs::path path=value;const std::string folder=path.parent_path().filename().string();
    return path.filename()=="shortcut-hook.so"&&path.parent_path().parent_path()=="/tmp"&&folder.rfind("mpc-usb-session.",0)==0&&folder.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos;
}
static std::string existingPreload(const std::string& service){
    const std::string pid=mainPid(service);int fd=open(("/proc/"+pid+"/environ").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,"Read MPC process environment");
    std::string environment;char buffer[4096];bool valid=true;ssize_t size;
    while((size=read(fd,buffer,sizeof buffer))!=0){if(size<0){if(errno==EINTR)continue;valid=false;break;}if(environment.size()+size>1024*1024){valid=false;break;}environment.append(buffer,size);}
    close(fd);if(!valid||(!environment.empty()&&environment.back()!=0))throw std::runtime_error("Invalid or oversized MPC process environment");
    if(mainPid(service)!=pid)throw std::runtime_error("MPC service changed while reading its environment; retry opening the manager");
    std::string preload;bool found=false;
    for(size_t start=0;start<environment.size();){const size_t end=environment.find('\0',start);const std::string entry=environment.substr(start,end-start);start=end+1;if(entry.rfind("LD_PRELOAD=",0)!=0)continue;if(found)throw std::runtime_error("Duplicate MPC preload environment");found=true;preload=entry.substr(11);}
    if(preload.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Unsupported newline in MPC preload environment");
    std::vector<std::string> libraries;bool stale=false;
    for(size_t start=0;start<preload.size();){start=preload.find_first_not_of(" :",start);if(start==std::string::npos)break;size_t end=preload.find_first_of(" :",start);if(end==std::string::npos)end=preload.size();const std::string library=preload.substr(start,end-start);if(ownHook(library))stale=true;else libraries.push_back(library);start=end;}
    if(stale){preload.clear();for(const auto& library:libraries){if(!preload.empty())preload+=' ';preload+=library;}}
    return preload;
}
static std::string environmentValue(const std::string& value){std::string result="\"";for(char byte:value){if(byte=='\\'||byte=='\"')result+='\\';result+=byte;}return result+'\"';}
static void writeEnvironment(const fs::path& path,const std::string& value){
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);require(fd>=0,"Create shortcut environment");size_t written=0;
    while(written<value.size()){const ssize_t size=write(fd,value.data()+written,value.size()-written);if(size<0&&errno==EINTR)continue;if(size<=0){close(fd);throw std::runtime_error("Write shortcut environment failed");}written+=size;}
    require(close(fd)==0,"Close shortcut environment");
}
static sockaddr_un address(const std::string& path){sockaddr_un result{};result.sun_family=AF_UNIX;require(path.size()<sizeof result.sun_path,"Socket path too long");strcpy(result.sun_path,path.c_str());return result;}
static int request(const std::string& path,char message='O'){int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|(message=='Q'||message=='H'?SOCK_NONBLOCK:0),0);if(fd<0)return 1;auto destination=address(path);const auto result=sendto(fd,&message,1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&destination),sizeof destination);close(fd);return result==1?0:1;}
static int cancelPrepared(const fs::path& base){
    require(getuid()==0,"Root required");
    require(base.parent_path()=="/tmp"&&base.filename().string().rfind("mpc-usb-session.",0)==0&&base.filename().string().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"Invalid prepared session folder");
    const uint64_t deadline=nowMs()+5000;bool sent=false;
    do {
        struct stat folder{};if(lstat(base.c_str(),&folder)<0){if(errno==ENOENT)return 0;throw std::runtime_error("Read prepared session folder failed");}
        require(S_ISDIR(folder.st_mode)&&folder.st_uid==0&&(folder.st_mode&0022)==0,"Unsafe prepared session folder");
        if(!sent){struct stat socket{};const fs::path path=base/"shortcut.sock";
            if(lstat(path.c_str(),&socket)==0){require(S_ISSOCK(socket.st_mode)&&socket.st_uid==0&&(socket.st_mode&0777)==0600&&socket.st_nlink==1,"Unsafe prepared session socket");sent=request(path.string(),'Q')==0;}
            else require(errno==ENOENT,"Read prepared session socket");
        }
        usleep(25000);
    }while(nowMs()<deadline);
    return 1;
}
struct Session {
    fs::path usb,base,dropin,state="/run/mpc-usb-manager-session";
    std::string service,port;
    int lockFd=-1,socketFd=-1,notifyFd=-1,timerFd=-1,signalFd=-1;
    pid_t child=0;
    bool configured=false,ownsState=false,ownsBase=false,armOnly=false,opened=false;
    HoldShortcut hold;
    Session(char** argv,bool arm):usb(argv[1]),base(argv[2]),service(argv[3]),port(argv[4]),armOnly(arm){
        require(getuid()==0,"Root required");require(service=="acvs"||service=="inmusic-mpc","Invalid MPC service");
        require(base.parent_path()=="/tmp"&&base.filename().string().rfind("mpc-usb-session.",0)==0,"Invalid session folder");
        require(base.filename().string().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"Invalid session folder characters");
        require(port.size()<32&&port.rfind("hw:",0)==0&&port.find_first_not_of("hw:0123456789,")==std::string::npos,"Invalid panel port");
    }
    ~Session(){cleanup();}
    void checkpoint() noexcept {std::error_code ec;if(fs::is_regular_file(usb/".mpc-manager-id",ec))fs::copy_file(base/"session.log",usb/"shortcut-result.txt",fs::copy_options::overwrite_existing,ec);}
    void cleanup() noexcept {
        try {
            if(configured){fs::remove(dropin);std::error_code ec;fs::remove(dropin.parent_path(),ec);command({"systemctl","daemon-reload"});configured=false;}
            if(child){
                kill(child,SIGTERM);while(waitpid(child,nullptr,0)<0&&errno==EINTR){}child=0;
                if(command({"systemctl","is-active","--quiet",service})!=0
                    &&command({"systemctl","start","--no-block",service})!=0)
                    std::cerr<<"MPC application recovery request failed\n";
            }
            if(ownsState)fs::remove(state);
            checkpoint();
            if(ownsBase)fs::remove_all(base);
        }catch(const std::exception& error){std::cerr<<"Shortcut cleanup: "<<error.what()<<'\n';}
        for(int fd:{socketFd,notifyFd,timerFd,signalFd,lockFd})if(fd>=0)close(fd);
    }
    void timer(){itimerspec value{};if(hold.deadline){value.it_value.tv_sec=hold.deadline/1000;value.it_value.tv_nsec=(hold.deadline%1000)*1000000;}require(timerfd_settime(timerFd,TFD_TIMER_ABSTIME,&value,nullptr)==0,"Set hold timer");}
    void launch(){
        if(child||!fs::is_regular_file(usb/".mpc-manager-id"))return;
        hold={};timer();
        std::string templatePath="/tmp/mpc-usb-manager.XXXXXX";std::vector<char> pattern(templatePath.begin(),templatePath.end());pattern.push_back(0);char* made=mkdtemp(pattern.data());require(made!=nullptr,"Create manager session");fs::path work=made;
        try{for(const char* name:{"manager","font.ttf"})fs::copy_file(base/name,work/name);require(chmod((work/"manager").c_str(),0755)==0&&chmod((work/"font.ttf").c_str(),0644)==0,"Set manager file permissions");std::ofstream(work/"session.log");
            const pid_t pid=fork();require(pid>=0,"Start manager");child=pid;opened=true;if(!child){sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,nullptr);execl("/bin/sh","sh",(base/"launch.sh").c_str(),usb.c_str(),work.c_str(),nullptr);_exit(127);}
        }catch(...){fs::remove_all(work);throw;}
        std::cerr<<"Opening USB manager\n";checkpoint();
    }
    void run(){
        lockFd=open("/run/mpc-usb-manager.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);require(lockFd>=0&&flock(lockFd,LOCK_EX|LOCK_NB)==0,"Manager session already active");
        ownsBase=true;
        int log=open((base/"session.log").c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);require(log>=0,"Open shortcut log");dup2(log,STDERR_FILENO);close(log);
        std::string preload=existingPreload(service);if(!preload.empty())preload+=' ';preload+=(base/"shortcut-hook.so").string();
        const std::vector<std::string> assignments={"LD_PRELOAD="+preload,"MPC_PANEL_PORT="+port,"MPC_SHORTCUT_SOCKET="+(base/"shortcut.sock").string()};
        for(const auto& unset:propertyWords(serviceProperty(service,"UnsetEnvironment")))for(const auto& assignment:assignments)if(unset==assignment||unset==assignment.substr(0,assignment.find('=')))throw std::runtime_error("MPC service explicitly unsets a required shortcut variable");
        writeEnvironment(base/"shortcut.env","LD_PRELOAD="+environmentValue(preload)+"\nMPC_PANEL_PORT="+environmentValue(port)+"\nMPC_SHORTCUT_SOCKET="+environmentValue((base/"shortcut.sock").string())+"\n");
        sigset_t signals;sigemptyset(&signals);for(int signal:{SIGTERM,SIGINT,SIGHUP,SIGCHLD})sigaddset(&signals,signal);require(sigprocmask(SIG_BLOCK,&signals,nullptr)==0,"Block supervisor signals");
        signalFd=signalfd(-1,&signals,SFD_CLOEXEC|SFD_NONBLOCK);require(signalFd>=0,"Create signal descriptor");
        socketFd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);require(socketFd>=0,"Create input socket");auto socketAddress=address((base/"shortcut.sock").string());require(bind(socketFd,reinterpret_cast<sockaddr*>(&socketAddress),sizeof socketAddress)==0,"Bind input socket");chmod((base/"shortcut.sock").c_str(),0600);
        timerFd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);require(timerFd>=0,"Create hold timer");
        notifyFd=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);require(notifyFd>=0&&inotify_add_watch(notifyFd,usb.c_str(),IN_UNMOUNT|IN_DELETE_SELF|IN_MOVE_SELF|IN_DELETE)>=0,"Watch USB removal");
        require(fs::is_regular_file(usb/".mpc-manager-id"),"USB removed before startup");
        {std::ofstream file(state);file<<base.string()<<'\n'<<usb.string()<<'\n';require(bool(file),"Write manager session state");}chmod(state.c_str(),0600);ownsState=true;
        dropin=fs::path("/run/systemd/system")/(service+".service.d")/"99-mpc-usb-shortcut.conf";require(!fs::exists(dropin),"Shortcut drop-in already exists");fs::create_directories(dropin.parent_path());configured=true;
        {std::ofstream file(dropin);file<<"[Service]\nEnvironmentFile="<<(base/"shortcut.env").string()<<"\n";require(bool(file),"Write shortcut startup setting");}
        require(command({"systemctl","daemon-reload"})==0,"Reload MPC service");
        const std::string files=serviceProperty(service,"EnvironmentFiles"),expected=(base/"shortcut.env").string()+" (ignore_errors=no)";
        require(files.size()>=expected.size()&&files.compare(files.size()-expected.size(),expected.size(),expected)==0,"Shortcut environment file must be last in the MPC service");
        if(armOnly)writeEnvironment(base/"ready","READY\n");
        if(!armOnly)launch();
        pollfd descriptors[]={{socketFd,POLLIN,0},{timerFd,POLLIN,0},{notifyFd,POLLIN,0},{signalFd,POLLIN,0}};
        bool stopping=false;
        while(!stopping){
            const int ready=poll(descriptors,4,-1);if(ready<0&&errno==EINTR)continue;require(ready>=0,"Wait for shortcut events");
            if(descriptors[2].revents){alignas(inotify_event)char events[4096];ssize_t count;while((count=read(notifyFd,events,sizeof events))>0){for(ssize_t offset=0;offset<count;){auto* event=reinterpret_cast<inotify_event*>(events+offset);if(event->mask&(IN_UNMOUNT|IN_DELETE_SELF|IN_MOVE_SELF|IN_IGNORED))stopping=true;if((event->mask&IN_DELETE)&&event->len&&strcmp(event->name,".mpc-manager-id")==0)stopping=true;offset+=sizeof(inotify_event)+event->len;}}}
            if(stopping)break;
            if(descriptors[3].revents){signalfd_siginfo event{};while(read(signalFd,&event,sizeof event)==sizeof event){if(event.ssi_signo==SIGCHLD){if(child&&waitpid(child,nullptr,WNOHANG)==child){child=0;hold={};timer();std::cerr<<"MPC resumed; SHIFT + MENU hold ready\n";checkpoint();}}else stopping=true;}}
            if(stopping)break;
            if(descriptors[0].revents){unsigned char message[4];ssize_t count;while((count=recv(socketFd,message,sizeof message,0))>=0){if(count==1&&message[0]=='Q'&&armOnly&&!opened&&!child){stopping=true;break;}else if(count==1&&message[0]=='H'&&armOnly&&!child){stopping=true;break;}else if(count==1&&message[0]=='O'){launch();}else if(count==1&&message[0]=='R'){hold={};timer();}else if(count==3&&message[0]=='B'&&message[2]<=1&&!child){hold.button(message[1],message[2]!=0,nowMs());timer();}}}
            if(stopping)break;
            if(descriptors[1].revents){uint64_t expirations;if(read(timerFd,&expirations,sizeof expirations)==sizeof expirations&&hold.expire(nowMs())){launch();}}
        }
        std::cerr<<"USB shortcut session closing\n";
    }
};
int main(int argc,char** argv){try{if(argc==3&&strcmp(argv[1],"--request")==0)return request(argv[2]);if(argc==3&&strcmp(argv[1],"--handoff")==0)return request(argv[2],'H');if(argc==3&&strcmp(argv[1],"--cancel-prepared")==0)return cancelPrepared(argv[2]);if(argc!=5&&(argc!=6||strcmp(argv[5],"--arm-only")!=0))throw std::runtime_error("Usage: shortcut USB RAM_SESSION SERVICE PANEL_PORT [--arm-only]");Session session(argv,argc==6);try{session.run();}catch(const std::exception& error){std::cerr<<"Shortcut error: "<<error.what()<<'\n';return 1;}return 0;}catch(const std::exception& error){std::cerr<<"Shortcut error: "<<error.what()<<'\n';return 1;}}
