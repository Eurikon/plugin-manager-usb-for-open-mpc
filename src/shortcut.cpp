#include "shortcut_state.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <csignal>
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
static sockaddr_un address(const std::string& path){sockaddr_un result{};result.sun_family=AF_UNIX;require(path.size()<sizeof result.sun_path,"Socket path too long");strcpy(result.sun_path,path.c_str());return result;}
static int request(const std::string& path){int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 1;auto destination=address(path);const char open='O';const auto result=sendto(fd,&open,1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&destination),sizeof destination);close(fd);return result==1?0:1;}
struct Session {
    fs::path usb,base,dropin,state="/run/mpc-usb-manager-session";
    std::string service,port;
    int lockFd=-1,socketFd=-1,notifyFd=-1,timerFd=-1,signalFd=-1;
    pid_t child=0;
    bool configured=false,ownsState=false,ownsBase=false,armOnly=false;
    HoldShortcut hold;
    Session(char** argv,bool arm):usb(argv[1]),base(argv[2]),service(argv[3]),port(argv[4]),armOnly(arm){
        require(getuid()==0,"Root required");require(service=="acvs"||service=="inmusic-mpc","Invalid MPC service");
        require(base.parent_path()=="/tmp"&&base.filename().string().rfind("mpc-usb-session.",0)==0,"Invalid session folder");
        require(port.size()<32&&port.rfind("hw:",0)==0&&port.find_first_not_of("hw:0123456789,")==std::string::npos,"Invalid panel port");
    }
    ~Session(){cleanup();}
    void checkpoint() noexcept {std::error_code ec;if(fs::is_regular_file(usb/".mpc-manager-id",ec))fs::copy_file(base/"session.log",usb/"shortcut-result.txt",fs::copy_options::overwrite_existing,ec);}
    void cleanup() noexcept {
        try {
            if(configured){fs::remove(dropin);std::error_code ec;fs::remove(dropin.parent_path(),ec);command({"systemctl","daemon-reload"});configured=false;}
            if(child){kill(child,SIGTERM);while(waitpid(child,nullptr,0)<0&&errno==EINTR){}child=0;}
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
            const pid_t pid=fork();require(pid>=0,"Start manager");child=pid;if(!child){sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,nullptr);execl("/bin/sh","sh",(base/"launch.sh").c_str(),usb.c_str(),work.c_str(),nullptr);_exit(127);}
        }catch(...){fs::remove_all(work);throw;}
        std::cerr<<"Opening USB manager\n";checkpoint();
    }
    void run(){
        lockFd=open("/run/mpc-usb-manager.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);require(lockFd>=0&&flock(lockFd,LOCK_EX|LOCK_NB)==0,"Manager session already active");
        ownsBase=true;
        int log=open((base/"session.log").c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);require(log>=0,"Open shortcut log");dup2(log,STDERR_FILENO);close(log);
        sigset_t signals;sigemptyset(&signals);for(int signal:{SIGTERM,SIGINT,SIGHUP,SIGCHLD})sigaddset(&signals,signal);require(sigprocmask(SIG_BLOCK,&signals,nullptr)==0,"Block supervisor signals");
        signalFd=signalfd(-1,&signals,SFD_CLOEXEC|SFD_NONBLOCK);require(signalFd>=0,"Create signal descriptor");
        socketFd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);require(socketFd>=0,"Create input socket");auto socketAddress=address((base/"shortcut.sock").string());require(bind(socketFd,reinterpret_cast<sockaddr*>(&socketAddress),sizeof socketAddress)==0,"Bind input socket");chmod((base/"shortcut.sock").c_str(),0600);
        timerFd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);require(timerFd>=0,"Create hold timer");
        notifyFd=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);require(notifyFd>=0&&inotify_add_watch(notifyFd,usb.c_str(),IN_UNMOUNT|IN_DELETE_SELF|IN_MOVE_SELF|IN_DELETE)>=0,"Watch USB removal");
        require(fs::is_regular_file(usb/".mpc-manager-id"),"USB removed before startup");
        {std::ofstream file(state);file<<base.string()<<'\n'<<usb.string()<<'\n';require(bool(file),"Write manager session state");}chmod(state.c_str(),0600);ownsState=true;
        dropin=fs::path("/run/systemd/system")/(service+".service.d")/"99-mpc-usb-shortcut.conf";require(!fs::exists(dropin),"Shortcut drop-in already exists");fs::create_directories(dropin.parent_path());configured=true;
        {std::ofstream file(dropin);file<<"[Service]\nEnvironment=\"LD_PRELOAD="<<(base/"shortcut-hook.so").string()<<"\"\nEnvironment=\"MPC_PANEL_PORT="<<port<<"\"\nEnvironment=\"MPC_SHORTCUT_SOCKET="<<(base/"shortcut.sock").string()<<"\"\n";require(bool(file),"Write shortcut startup setting");}
        require(command({"systemctl","daemon-reload"})==0,"Reload MPC service");if(!armOnly)launch();
        pollfd descriptors[]={{socketFd,POLLIN,0},{timerFd,POLLIN,0},{notifyFd,POLLIN,0},{signalFd,POLLIN,0}};
        bool stopping=false;
        while(!stopping){
            const int ready=poll(descriptors,4,-1);if(ready<0&&errno==EINTR)continue;require(ready>=0,"Wait for shortcut events");
            if(descriptors[2].revents){alignas(inotify_event)char events[4096];ssize_t count;while((count=read(notifyFd,events,sizeof events))>0){for(ssize_t offset=0;offset<count;){auto* event=reinterpret_cast<inotify_event*>(events+offset);if(event->mask&(IN_UNMOUNT|IN_DELETE_SELF|IN_MOVE_SELF|IN_IGNORED))stopping=true;if((event->mask&IN_DELETE)&&event->len&&strcmp(event->name,".mpc-manager-id")==0)stopping=true;offset+=sizeof(inotify_event)+event->len;}}}
            if(stopping)break;
            if(descriptors[3].revents){signalfd_siginfo event{};while(read(signalFd,&event,sizeof event)==sizeof event){if(event.ssi_signo==SIGCHLD){if(child&&waitpid(child,nullptr,WNOHANG)==child){child=0;hold={};timer();std::cerr<<"MPC resumed; SHIFT + MENU hold ready\n";checkpoint();}}else stopping=true;}}
            if(stopping)break;
            if(descriptors[0].revents){unsigned char message[4];ssize_t count;while((count=recv(socketFd,message,sizeof message,0))>=0){if(count==1&&message[0]=='O'){launch();}else if(count==1&&message[0]=='R'){hold={};timer();}else if(count==3&&message[0]=='B'&&message[2]<=1&&!child){hold.button(message[1],message[2]!=0,nowMs());timer();}}}
            if(descriptors[1].revents){uint64_t expirations;if(read(timerFd,&expirations,sizeof expirations)==sizeof expirations&&hold.expire(nowMs())){launch();}}
        }
        std::cerr<<"USB shortcut session closing\n";
    }
};
int main(int argc,char** argv){try{if(argc==3&&strcmp(argv[1],"--request")==0)return request(argv[2]);if(argc!=5&&(argc!=6||strcmp(argv[5],"--arm-only")!=0))throw std::runtime_error("Usage: shortcut USB RAM_SESSION SERVICE PANEL_PORT [--arm-only]");Session session(argv,argc==6);try{session.run();}catch(const std::exception& error){std::cerr<<"Shortcut error: "<<error.what()<<'\n';return 1;}return 0;}catch(const std::exception& error){std::cerr<<"Shortcut error: "<<error.what()<<'\n';return 1;}}
