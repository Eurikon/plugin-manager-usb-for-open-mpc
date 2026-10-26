#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <iterator>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs=std::filesystem;
static const fs::path root="/tmp/shortcut-test",base="/tmp/mpc-usb-session.test";
static fs::path usb=root/"usb";
static const fs::path dropin="/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf",foreignDropin=dropin.parent_path()/"20-addins.conf",foreignEnvironment=root/"existing.env",processEnvironment="/proc/4242/environ";
static const std::string foreignDropinText="[Service]\nEnvironment=\"LD_PRELOAD=/unit-setting.so\"\nEnvironmentFile=/tmp/shortcut-test/existing.env\n",foreignEnvironmentText="LD_PRELOAD=\"/file-setting.so\"\nOTHER=value\n";
static std::string contents(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
static void write(const fs::path& path,const std::string& value){std::ofstream file(path,std::ios::binary);file.write(value.data(),value.size());assert(bool(file));}
static void environment(const std::string& value){fs::create_directories(processEnvironment.parent_path());fs::remove(processEnvironment);write(processEnvironment,value);}
static std::string preload(const std::string& value){std::string result="HOME=/root";result+='\0';result+="LD_PRELOAD="+value;result+='\0';result+="UNCHANGED=keep";result+='\0';return result;}
static void preserved(){assert(contents(foreignDropin)==foreignDropinText&&contents(foreignEnvironment)==foreignEnvironmentText);}
static std::string field(const std::string& name){
    const std::string text=contents(base/"shortcut.env"),prefix=name+"=\"";size_t position=text.find(prefix);assert(position!=std::string::npos&&(position==0||text[position-1]=='\n'));position+=prefix.size();std::string result;
    for(;position<text.size();++position){const char value=text[position];if(value=='"'){assert(position+1==text.size()||text[position+1]=='\n');return result;}if(value=='\\'&&position+1<text.size()&&(text[position+1]=='\\'||text[position+1]=='"'))result+=text[++position];else result+=value;}
    assert(false);return {};
}
static void configured(const std::string& expected){
    assert(contents(dropin)=="[Service]\nEnvironmentFile="+(base/"shortcut.env").string()+"\n");
    assert(field("LD_PRELOAD")==expected&&field("MPC_PANEL_PORT")=="hw:1,0,1"&&field("MPC_SHORTCUT_SOCKET")=="/tmp/mpc-usb-session.test/shortcut.sock");
    struct stat mode{};assert(stat((base/"shortcut.env").c_str(),&mode)==0&&(mode.st_mode&0777)==0600&&mode.st_uid==0);preserved();
}
static int launches(){std::ifstream file(root/"launches");std::string line;int result=0;while(std::getline(file,line))++result;return result;}
template<class Predicate> static void until(Predicate predicate){for(int i=0;i<400;++i){if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(25));}throw std::runtime_error("Timed out waiting for supervisor");}
static void send(const unsigned char* bytes,size_t size){int fd=socket(AF_UNIX,SOCK_DGRAM,0);assert(fd>=0);sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,(base/"shortcut.sock").c_str());assert(sendto(fd,bytes,size,0,reinterpret_cast<sockaddr*>(&address),sizeof address)==ssize_t(size));close(fd);}
static void button(unsigned char note,bool down){const unsigned char bytes[]={'B',note,(unsigned char)down};send(bytes,sizeof bytes);}
static pid_t start(bool armOnly=false){pid_t pid=fork();assert(pid>=0);if(!pid){execl((root/"shortcut").c_str(),"shortcut",usb.c_str(),base.c_str(),"acvs","hw:1,0,1",armOnly?"--arm-only":nullptr,nullptr);_exit(127);}return pid;}
static pid_t cancel(const fs::path& path=base){pid_t pid=fork();assert(pid>=0);if(!pid){execl((root/"shortcut").c_str(),"shortcut","--cancel-prepared",path.c_str(),nullptr);_exit(127);}return pid;}
static pid_t handoff(){pid_t pid=fork();assert(pid>=0);if(!pid){execl((root/"shortcut").c_str(),"shortcut","--handoff",(base/"shortcut.sock").c_str(),nullptr);_exit(127);}return pid;}
static void returned(pid_t pid,int expected=0){int status=0;until([&]{return waitpid(pid,&status,WNOHANG)==pid;});assert(WIFEXITED(status)&&WEXITSTATUS(status)==expected);}
static void exited(pid_t pid,int expected=0){returned(pid,expected);assert(!fs::exists(base)&&!fs::exists("/run/mpc-usb-manager-session")&&!fs::exists(dropin));preserved();}
static void prepared(const std::string& original=""){fs::create_directories(usb);std::ofstream(usb/".mpc-manager-id")<<"test\n";fs::create_directories(base);for(const char* name:{"shortcut","shortcut-hook.so","manager","font.ttf","launch.sh"})fs::copy_file(root/name,base/name,fs::copy_options::overwrite_existing);fs::permissions(base/"manager",fs::perms::owner_all);fs::permissions(base/"shortcut",fs::perms::owner_all);fs::permissions(base/"font.ttf",fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read|fs::perms::others_read);environment(preload(original));write(root/"main-pid","4242\n");write(root/"service-active","1\n");for(const char* name:{"pid-race","pid-queried","fail-reload","fail-show","late-envfile","unset-variable","hold-pid-query","pid-query-entered","hold-environment-check","environment-check-entered","fail-restore-start"})fs::remove(root/name);}
static void armed(){until([]{return contents(base/"ready")=="READY\n";});struct stat mode{};assert(lstat((base/"ready").c_str(),&mode)==0&&S_ISREG(mode.st_mode)&&(mode.st_mode&0777)==0600&&mode.st_uid==0&&mode.st_nlink==1);}
int main(){try{
    fs::create_directories(dropin.parent_path());write(foreignDropin,foreignDropinText);write(foreignEnvironment,foreignEnvironmentText);
    const std::string addins=(root/"shortcut-addin-first.so").string()+":"+(root/"shortcut-addin-second.so").string(),hook=(base/"shortcut-hook.so").string();
    prepared(addins);const std::string originalEnvironment=contents(processEnvironment);pid_t pid=start();until([]{return launches()==1&&fs::exists(base/"shortcut.sock");});configured(addins+" "+hook);assert(contents(processEnvironment)==originalEnvironment&&!fs::exists(base/"ready"));std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout<<"PASS: captured addin order retained, hook appended last and foreign configuration untouched\n";
    returned(handoff());std::this_thread::sleep_for(std::chrono::milliseconds(100));assert(kill(pid,0)==0);configured(addins+" "+hook);
    std::cout<<"PASS: handoff leaves non-arm-only supervisor active\n";
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
    prepared();pid=start(true);armed();std::this_thread::sleep_for(std::chrono::milliseconds(500));assert(launches()==5);
    configured(hook);
    button(49,true);button(123,true);std::this_thread::sleep_for(std::chrono::milliseconds(400));button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::milliseconds(2100));assert(launches()==5);
    fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: arm-only waits without opening; short chord and idle removal do not open manager\n";
    prepared();pid=start(true);armed();returned(handoff());exited(pid);assert(launches()==5);
    std::cout<<"PASS: idle arm-only handoff clears runtime state and cache without opening manager\n";
    prepared();pid=start(true);armed();button(49,true);button(123,true);until([]{return launches()==6;});button(123,false);button(49,false);std::this_thread::sleep_for(std::chrono::seconds(1));returned(cancel(),1);assert(kill(pid,0)==0);configured(hook);fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: armed hold launches manager, then cancellation preserves session until removal\n";
    prepared();write(root/"hold-environment-check","1\n");pid=start(true);until([]{return fs::exists(root/"environment-check-entered");});assert(fs::exists(base/"shortcut.sock")&&fs::exists(dropin)&&!fs::exists(base/"ready")&&launches()==6);fs::remove(root/"hold-environment-check");armed();configured(hook);returned(cancel());exited(pid);assert(launches()==6);
    std::cout<<"PASS: readiness waits for final environment check; prepared cancellation cleans without opening\n";
    prepared();write(root/"hold-pid-query","1\n");pid=start(true);until([]{return fs::exists(root/"pid-query-entered");});assert(!fs::exists(base/"shortcut.sock"));const pid_t cancelling=cancel();std::this_thread::sleep_for(std::chrono::milliseconds(100));fs::remove(root/"hold-pid-query");returned(cancelling);exited(pid);assert(launches()==6);
    std::cout<<"PASS: cancellation before socket creation waits for supervisor initialization\n";
    prepared();pid=start(true);armed();send(&open,1);until([]{return launches()==7;});const unsigned char quit='Q';send(&quit,1);std::this_thread::sleep_for(std::chrono::seconds(1));returned(cancel(),1);assert(kill(pid,0)==0&&launches()==7);configured(hook);fs::remove(usb/".mpc-manager-id");exited(pid);
    std::cout<<"PASS: open request permanently disables prepared cancellation, including after GUI exit\n";
    prepared();write(base/"shortcut.sock","foreign\n");returned(cancel(),1);assert(contents(base/"shortcut.sock")=="foreign\n");fs::remove_all(base);fs::create_directory_symlink(root,base);returned(cancel(),1);assert(fs::is_symlink(base));fs::remove(base);returned(cancel(root),1);returned(cancel());
    std::cout<<"PASS: cancellation rejects foreign paths, symlink directories and non-socket files\n";
    const std::string quoted=addins+" /tmp/quoted\"path\\module.so:$ORIGIN/`literal`:$(touch /tmp/shortcut-test/evaluated)";
    prepared(quoted);pid=start(true);armed();configured(quoted+" "+hook);assert(!fs::exists(root/"evaluated"));kill(pid,SIGTERM);exited(pid);
    std::cout<<"PASS: quoted preload bytes retained without shell evaluation; termination restores original configuration\n";
    prepared((root/"shortcut-addin-first.so").string()+":/tmp/mpc-usb-session.old123/shortcut-hook.so "+(root/"shortcut-addin-second.so").string());pid=start(true);armed();
    const std::string filtered=field("LD_PRELOAD");assert(filtered.find("/tmp/mpc-usb-session.old123/")==std::string::npos&&filtered.find("shortcut-addin-first.so")<filtered.find("shortcut-addin-second.so")&&filtered.find("shortcut-addin-second.so")<filtered.find(hook)&&filtered.find(hook)==filtered.rfind(hook));preserved();kill(pid,SIGTERM);exited(pid);
    std::cout<<"PASS: stale PMU hook removed without reordering foreign addins or appending twice\n";
    prepared();const char noPreload[]="HOME=/root\0OTHER=value\0";environment(std::string(noPreload,sizeof noPreload-1));pid=start(true);armed();configured(hook);kill(pid,SIGTERM);exited(pid);
    std::cout<<"PASS: absent LD_PRELOAD retains stock startup\n";
    prepared(addins);write(root/"unset-variable","LD_PRELOAD=/different.so MPC_PANEL_PORT=hw:9,9,9\n");pid=start(true);armed();configured(addins+" "+hook);kill(pid,SIGTERM);exited(pid);
    std::cout<<"PASS: unrelated and nonmatching value-specific environment removals retained\n";
    for(const std::string failure:{"zero-pid","malformed-pid","query-failure","missing-environ","symlink-environ","truncated-environ","oversized-environ","duplicate-preload","newline-preload","changed-pid","reload","unset-variable","unset-preload-assignment","unset-port","unset-socket","late-envfile"}){
        prepared(addins);
        if(failure=="zero-pid")write(root/"main-pid","0\n");
        else if(failure=="malformed-pid")write(root/"main-pid","4242;false\n");
        else if(failure=="query-failure")write(root/"fail-show","1\n");
        else if(failure=="missing-environ")fs::remove(processEnvironment);
        else if(failure=="symlink-environ"){write(root/"environment-target",preload(addins));fs::remove(processEnvironment);fs::create_symlink(root/"environment-target",processEnvironment);}
        else if(failure=="truncated-environ")environment("LD_PRELOAD="+addins);
        else if(failure=="oversized-environ")environment(preload(std::string(1024*1024,'x')));
        else if(failure=="duplicate-preload")environment(preload(addins)+preload("/another.so"));
        else if(failure=="newline-preload")environment(preload(addins+"\nLD_PRELOAD=/injected.so"));
        else if(failure=="changed-pid")write(root/"pid-race","1\n");
        else if(failure=="unset-variable")write(root/"unset-variable","LD_PRELOAD\n");
        else if(failure=="unset-preload-assignment")write(root/"unset-variable","\"LD_PRELOAD="+addins+" "+hook+"\"\n");
        else if(failure=="unset-port")write(root/"unset-variable","MPC_PANEL_PORT=hw:1,0,1\n");
        else if(failure=="unset-socket")write(root/"unset-variable","MPC_SHORTCUT_SOCKET=/tmp/mpc-usb-session.test/shortcut.sock\n");
        else if(failure=="late-envfile")write(root/"late-envfile","1\n");
        else write(root/"fail-reload","1\n");
        pid=start(true);exited(pid,1);assert(launches()==7);std::cout<<"PASS: "<<failure<<" fails before opening manager and preserves addin configuration\n";
    }
    usb=root/"MPC SD"/"OPEN MPC"/"Plugin Manager";
    prepared();write(usb/"plugin-history.json","retained\n");pid=start(true);armed();send(&open,1);until([]{return launches()==8;});
    fs::remove(usb/".mpc-manager-id");exited(pid);assert(contents(usb/"plugin-history.json")=="retained\n");
    std::cout<<"PASS: nested SD source launches from RAM; removal restores MPC and preserves saved data\n";
    prepared();write(root/"hold-manager","1\n");pid=start(true);armed();send(&open,1);until([]{return launches()==9;});
    returned(handoff());std::this_thread::sleep_for(std::chrono::milliseconds(100));assert(kill(pid,0)==0);configured(hook);assert(fs::exists("/run/mpc-usb-manager-session"));
    fs::remove(root/"hold-manager");until([]{return contents(base/"session.log").find("MPC resumed; SHIFT + MENU hold ready")!=std::string::npos;});assert(kill(pid,0)==0);configured(hook);returned(cancel(),1);assert(kill(pid,0)==0);
    returned(handoff());exited(pid);assert(launches()==9&&contents(usb/"plugin-history.json")=="retained\n");
    std::cout<<"PASS: live manager refuses handoff; Return keeps Q disabled and permits explicit idle handoff\n";
    prepared();write(root/"hold-manager","1\n");write(root/"fail-restore-start","1\n");
    pid=start(true);armed();send(&open,1);until([]{return launches()==10;});
    fs::remove(usb/".mpc-manager-id");exited(pid);
    assert(fs::exists(root/"service-active")&&contents(root/"recovery-requests")=="recovery\n"
           &&contents(usb/"plugin-history.json")=="retained\n");
    fs::remove(root/"hold-manager");fs::remove(root/"fail-restore-start");
    std::cout<<"PASS: SD uninstall recovers MPC when the return launcher fails to restart it\n";
    fs::remove(foreignDropin);fs::remove(foreignEnvironment);fs::remove_all(processEnvironment.parent_path());
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
