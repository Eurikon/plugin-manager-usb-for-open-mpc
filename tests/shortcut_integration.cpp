#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs=std::filesystem;
static const fs::path root="/tmp/shortcut-test",usb=root/"usb",base="/tmp/mpc-usb-session.test";
static int launches(){std::ifstream file(root/"launches");std::string line;int result=0;while(std::getline(file,line))++result;return result;}
template<class Predicate> static void until(Predicate predicate){for(int i=0;i<400;++i){if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(25));}throw std::runtime_error("Timed out waiting for supervisor");}
static void send(const unsigned char* bytes,size_t size){int fd=socket(AF_UNIX,SOCK_DGRAM,0);assert(fd>=0);sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,(base/"shortcut.sock").c_str());assert(sendto(fd,bytes,size,0,reinterpret_cast<sockaddr*>(&address),sizeof address)==ssize_t(size));close(fd);}
static void button(unsigned char note,bool down){const unsigned char bytes[]={'B',note,(unsigned char)down};send(bytes,sizeof bytes);}
static pid_t start(bool armOnly=false){pid_t pid=fork();assert(pid>=0);if(!pid){execl((root/"shortcut").c_str(),"shortcut",usb.c_str(),base.c_str(),"acvs","hw:1,0,1",armOnly?"--arm-only":nullptr,nullptr);_exit(127);}return pid;}
static void exited(pid_t pid){int status=0;until([&]{return waitpid(pid,&status,WNOHANG)==pid;});assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(!fs::exists(base)&&!fs::exists("/run/mpc-usb-manager-session")&&!fs::exists("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf"));}
static void prepared(){fs::create_directories(usb);std::ofstream(usb/".mpc-manager-id")<<"test\n";fs::create_directories(base);for(const char* name:{"shortcut","shortcut-hook.so","manager","font.ttf","launch.sh"})fs::copy_file(root/name,base/name,fs::copy_options::overwrite_existing);fs::permissions(base/"manager",fs::perms::owner_all);fs::permissions(base/"shortcut",fs::perms::owner_all);fs::permissions(base/"font.ttf",fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read|fs::perms::others_read);}
int main(){try{
    prepared();pid_t pid=start();until([]{return launches()==1&&fs::exists(base/"shortcut.sock");});std::this_thread::sleep_for(std::chrono::seconds(1));
    button(49,true);button(123,true);std::this_thread::sleep_for(std::chrono::milliseconds(400));button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::milliseconds(2100));assert(launches()==1);
    std::cout<<"PASS: short Browser chord does not reopen manager\n";
    button(49,true);button(123,true);until([]{return launches()==2;});button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout<<"PASS: two-second hold launches without another MIDI event\n";
    button(123,true);button(49,true);until([]{return launches()==3;});button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout<<"PASS: repeated reopening and reverse button order\n";
    pid_t requester=fork();assert(requester>=0);if(!requester){execl("/bin/sh","sh",(usb/"autorun/open.sh").c_str(),nullptr);_exit(127);}int requestStatus;assert(waitpid(requester,&requestStatus,0)==requester&&WIFEXITED(requestStatus)&&WEXITSTATUS(requestStatus)==0);
    const unsigned char open='O';send(&open,1);until([]{return launches()==4;});std::this_thread::sleep_for(std::chrono::seconds(1));assert(launches()==4);
    std::cout<<"PASS: duplicate requests while manager opens are ignored\n";
    fs::remove(usb/".mpc-manager-id");exited(pid);std::cout<<"PASS: idle USB removal clears hook configuration and RAM session\n";
    prepared();pid=start();until([]{return launches()==5;});fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: USB removal while manager is open restores MPC and clears RAM session\n";
    prepared();pid=start(true);until([]{return fs::exists(base/"shortcut.sock")&&fs::exists("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf");});std::this_thread::sleep_for(std::chrono::milliseconds(500));assert(launches()==5);
    button(49,true);button(123,true);std::this_thread::sleep_for(std::chrono::milliseconds(400));button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::milliseconds(2100));assert(launches()==5);
    fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: arm-only waits without opening; short chord and idle removal do not open manager\n";
    prepared();pid=start(true);until([]{return fs::exists(base/"shortcut.sock")&&fs::exists("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf");});button(49,true);button(123,true);until([]{return launches()==6;});button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::seconds(1));fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: armed two-second hold launches manager and removal clears RAM session\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
