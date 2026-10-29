#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "runtime_handoff.hpp"
#include "runtime_session.hpp"
#include "session_handoff.hpp"
#include "checksum.hpp"
#include "crypto.hpp"
#include "platform.hpp"
#include "startup/storage.h"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string read(const fs::path& path,size_t limit){
    const int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);need(fd>=0,"Cannot read runtime handover state");struct stat status{};bool valid=fstat(fd,&status)==0&&S_ISREG(status.st_mode)&&status.st_uid==0&&status.st_nlink==1&&(status.st_mode&0777)==0600&&status.st_size>=0&&uint64_t(status.st_size)<=limit;
    std::string text;std::array<char,4096> bytes{};while(valid){const auto size=::read(fd,bytes.data(),bytes.size());if(size<0&&errno==EINTR)continue;if(size<0||text.size()+size_t(size)>limit){valid=false;break;}if(!size)break;text.append(bytes.data(),size_t(size));}close(fd);need(valid,"Unsafe runtime handover state");return text;
}
void mark(const fs::path& file,const std::string& generation){
    const std::string text=generation+'\n';const int fd=open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);need(fd>=0,"Cannot record runtime handover progress");const bool ok=write(fd,text.data(),text.size())==ssize_t(text.size())&&fsync(fd)==0;close(fd);need(ok,"Cannot finish runtime handover progress");
}
bool marked(const fs::path& file,const std::string& generation){struct stat status{};if(lstat(file.c_str(),&status)!=0){need(errno==ENOENT,"Cannot inspect runtime handover progress");return false;}return read(file,65)==generation+'\n';}
void self(const NativeImageSpec& image){validateNativeImage(image);need(runningImageSha256()==image.sha256,"Another running program cannot handle this runtime handover");}
void helper(const fs::path& path){if(path.empty())return;need(pmu_helper_directory_valid(path.c_str()),"Unsupported handover helper location");
#if !defined(__aarch64__)
    need(path==PMU_HELPER_DIRECTORY,"Unsupported Gen 1 handover helper location");
#endif
}
void selection(const SessionHandoff& item,const std::string& publisher){helper(item.helper);SourceRuntime source(item.source,managerArchitecture(),publisher);need(source.choose(false,item.sourceIdentity).generation.id==item.generation,"Runtime handover selection changed");}
std::string quoted(const std::string& text){
    need(text.find('\0')==std::string::npos&&text.find_first_of("\r\n")==std::string::npos,"Invalid runtime service argument");std::string result="\"";for(char byte:text){if(byte=='%'||byte=='$')result+=byte;if(byte=='\\'||byte=='\"')result+='\\';result+=byte;}return result+'\"';
}
bool registered(const fs::path& base,const fs::path& source){struct stat status{};if(lstat("/run/mpc-usb-manager-session",&status)!=0){need(errno==ENOENT,"Cannot inspect running runtime cohort");return false;}try{return read("/run/mpc-usb-manager-session",8192)==base.string()+'\n'+source.string()+'\n';}catch(...){if(lstat("/run/mpc-usb-manager-session",&status)!=0&&errno==ENOENT)return false;throw;}}
void signal(const fs::path& base,char request){
    const auto path=base/"shortcut.sock";struct stat status{};if(lstat(path.c_str(),&status)!=0){need(errno==ENOENT,"Cannot inspect runtime handover socket");return;}need(S_ISSOCK(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0600&&status.st_nlink==1,"Unsafe runtime handover socket");sockaddr_un address{};address.sun_family=AF_UNIX;const auto name=path.string();need(name.size()<sizeof address.sun_path,"Invalid handover socket length");std::copy(name.begin(),name.end(),address.sun_path);const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);need(fd>=0,"Cannot contact runtime supervisor");const auto result=sendto(fd,&request,1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof address);const int error=errno;close(fd);need(result==1||error==ENOENT||error==ECONNREFUSED,"Cannot request runtime supervisor handover");
}
void waitUnit(const std::chrono::steady_clock::time_point& deadline){
    for(;;){const auto state=runtimeOutput({"/bin/systemctl","show","mpc-usb-manager.service","-pLoadState","--value"});if((state.first==0||state.first==4)&&state.second=="not-found\n")break;need(state.first==0&&state.second=="loaded\n","Cannot inspect retired manager service");need(std::chrono::steady_clock::now()<deadline,"Retired manager service was not released");usleep(25000);}
}
void retire(const SessionHandoff& item,const fs::path& base){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(registered(base,item.source)){signal(base,'H');need(std::chrono::steady_clock::now()<deadline,"Runtime supervisor did not finish its active operation");usleep(25000);}
    struct stat status{};while(lstat(base.c_str(),&status)==0){need(std::chrono::steady_clock::now()<deadline,"Runtime supervisor cleanup timed out");usleep(25000);}need(errno==ENOENT,"Cannot inspect retired runtime cohort");
    waitUnit(deadline);
}
void restore(const fs::path& path,const std::string& previous,const std::string& expected,const std::string& kind,const std::string& publisher,const std::string& profile={}){
    if(previous.empty())return;
    VersionStore store(path,geteuid(),[](const auto& file){return fileSha256(file);},{},false,[&](const auto& id,const auto&,const auto& approval){verifyAuthorization(publisher,kind,id,approval);},profile);const auto current=store.current();need(current&&(current->id==expected||current->id==previous),"Recovery refused a later selected version");if(current->id==expected)store.restore(previous,expected);
}
void helperHealth(const SessionHandoff& item,const std::string& publisher){
    if(item.helperGeneration.empty())return;
    VersionStore store(item.helper/".pmu-versions",geteuid(),[](const auto& file){return fileSha256(file);},{},false,[&](const auto& id,const auto&,const auto& approval){verifyAuthorization(publisher,"helper",id,approval);});const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    for(;;){const auto current=store.current();need(current&&current->id==item.helperGeneration,"The updated helper failed its startup check");if(!current->pending)return;need(std::chrono::steady_clock::now()<deadline,"Updated helper startup confirmation timed out");usleep(250000);}
}
}
void beginRuntimeHandoff(const fs::path& capsule,const std::string& publisher){
    need(geteuid()==0,"Runtime handover requires the manager service");const auto item=readSessionHandoff(capsule);selection(item,publisher);const auto incoming=preparedRuntimeWorker(item.newBase,publisher);self(incoming);validateNativeImage(item.rollbackWorker);need(registered(item.oldBase,item.source),"The previous runtime cohort is no longer active");
    const auto mpc=runtimeOutput({"/bin/systemctl","show",item.service,"-pMainPID","--value"});need(mpc.first==0&&mpc.second=="0\n","MPC resumed before the update handover");
    const auto unit="mpc-usb-manager-handoff-"+item.generation.substr(0,16);const auto recovery=quoted(item.rollbackWorker.executable.string())+" --pmu-recover-v1 "+quoted(capsule.string());
    need(runtimeCommand({"/usr/bin/systemd-run","--unit="+unit,"--collect","--property=Type=exec","--property=RuntimeMaxSec=180","--property=TimeoutStartSec=90","--property=TimeoutStopSec=45","--property=ExecStopPost="+recovery,incoming.executable.string(),"--pmu-finalize-v1",capsule.string()})==0,"Cannot start independent runtime handover");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(!marked(item.rollbackBase/"handoff.accepted",item.generation)){need(std::chrono::steady_clock::now()<deadline,"Runtime handover did not acknowledge readiness");usleep(25000);}
}
void finalizeRuntimeHandoff(const fs::path& capsule,const std::string& publisher){
    need(geteuid()==0,"Runtime handover requires the manager service");const auto item=readSessionHandoff(capsule);selection(item,publisher);self(preparedRuntimeWorker(item.newBase,publisher));validateNativeImage(item.rollbackWorker);mark(item.rollbackBase/"handoff.accepted",item.generation);
    retire(item,item.oldBase);selection(item,publisher);startPreparedRuntime(item.newBase,publisher);need(registered(item.newBase,item.source),"New runtime did not take ownership");
    if(item.openAfterHandoff)signal(item.newBase,'O');else{need(runtimeCommand({"/bin/systemctl","start",item.service},item.helper,std::chrono::seconds(45))==0,"MPC application did not resume after runtime handover");helperHealth(item,publisher);}
    mark(item.rollbackBase/"handoff.complete",item.generation);
}
void recoverRuntimeHandoff(const fs::path& capsule,const std::string& publisher){
    need(geteuid()==0,"Runtime recovery requires the manager service");const auto item=readSessionHandoff(capsule);self(item.rollbackWorker);helper(item.helper);
    if(marked(item.rollbackBase/"handoff.complete",item.generation)){fs::remove_all(item.rollbackBase);return;}
    try{
        if(!registered(item.oldBase,item.source))need(runtimeCommand({"/bin/systemctl","stop",item.service},item.helper,std::chrono::seconds(45))==0,"Cannot stop the failed updated MPC application before recovery");
        if(registered(item.newBase,item.source))retire(item,item.newBase);
        SourceRuntime source(item.source,managerArchitecture(),publisher);source.identity();need(source.identity()==item.sourceIdentity,"Recovery source was removed or replaced");
        // A RAM refresh must preserve storage's health decision, not revive its failed GUI.
        if(!item.openAfterHandoff)restore(item.source/"autorun/runtime/.pmu-versions",item.previousGeneration,item.generation,"runtime",publisher,managerArchitecture());
        if(!item.helper.empty())restore(item.helper/".pmu-versions",item.previousHelperGeneration,item.helperGeneration,"helper",publisher);
        if(!registered(item.oldBase,item.source)){waitUnit(std::chrono::steady_clock::now()+std::chrono::seconds(15));startPreparedRuntime(item.rollbackBase,publisher);}
    }catch(...){runtimeCommand({"/bin/systemctl","start",item.service},item.helper,std::chrono::seconds(45));throw;}
    need(runtimeCommand({"/bin/systemctl","start",item.service},item.helper,std::chrono::seconds(45))==0,"MPC application recovery failed");
}
}
