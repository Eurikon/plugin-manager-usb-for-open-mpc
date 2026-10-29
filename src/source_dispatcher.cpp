#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "source_runtime.hpp"
#include "update_worker.hpp"
#include "metadata.hpp"
#include "platform.hpp"
#include <publisher_key.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
namespace fs=std::filesystem;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {int value;explicit Fd(int fd):value(fd){}~Fd(){if(value>=0)close(value);}Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;};
bool cohortPath(const fs::path& value){const auto name=value.filename().string();return value.parent_path()=="/tmp"&&name.rfind("mpc-usb-session.",0)==0&&name.size()>16&&name.size()<80&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos;}
std::string read(const fs::path& path,size_t limit){
    Fd fd(open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW));need(fd.value>=0,"Cannot read manager session identity");struct stat status{};need(fstat(fd.value,&status)==0&&S_ISREG(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0600&&status.st_nlink==1&&status.st_size>=0&&uint64_t(status.st_size)<=limit,"Unsafe manager session identity");std::array<char,4096> bytes{};std::string text;
    for(;;){const auto count=::read(fd.value,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;need(count>=0&&text.size()+size_t(count)<=limit,"Invalid manager session identity");if(!count)break;text.append(bytes.data(),size_t(count));}return text;
}
void folder(const fs::path& base){struct stat status{};need(cohortPath(base)&&lstat(base.c_str(),&status)==0&&S_ISDIR(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0700,"Unsafe manager RAM cohort");}
fs::path existing(const fs::path& source,const pmu::RuntimeChoice& selected){
    struct stat status{};if(lstat("/run/mpc-usb-manager-session",&status)!=0){need(errno==ENOENT,"Cannot inspect existing manager session");return {};}
    const auto text=read("/run/mpc-usb-manager-session",8192);std::istringstream lines(text);std::string base,recorded,extra;need(std::getline(lines,base)&&std::getline(lines,recorded)&&!std::getline(lines,extra)&&text==base+'\n'+recorded+'\n',"Invalid existing manager session record");need(recorded==source.string(),"Another manager source is already active");folder(base);
    const auto context=pmu::parseMetadata(read(fs::path(base)/"runtime-context.json",8192));need(context.value("protocol",0)==1&&context.at("source_identity")==selected.identity&&context.at("source")==source.string(),"Active manager cohort belongs to another source");return base;
}
void open(const fs::path& base){
    folder(base);const fs::path path=base/"shortcut.sock";struct stat status{};need(lstat(path.c_str(),&status)==0&&S_ISSOCK(status.st_mode)&&status.st_uid==0&&(status.st_mode&0777)==0600&&status.st_nlink==1,"Manager supervisor socket is unavailable");sockaddr_un target{};target.sun_family=AF_UNIX;const auto name=path.string();need(name.size()<sizeof target.sun_path,"Manager supervisor socket name is too long");std::copy(name.begin(),name.end(),target.sun_path);Fd socket(::socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0));need(socket.value>=0,"Cannot contact manager supervisor");const char request='O';need(sendto(socket.value,&request,1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&target),sizeof target)==1,"Manager supervisor did not accept opening");
}
}
int main(int argc,char** argv){
    fs::path scratch;
    try{
        need(argc==4&&geteuid()==0,"Usage: source-dispatcher SOURCE HELPER prepare|open");const fs::path source=argv[1],helper=argv[2];const std::string mode=argv[3];need(mode=="prepare"||mode=="open","Unsupported manager opening mode");pmu::SourceRuntime runtime(source,managerArchitecture(),pmu::publisherKey);const auto selected=runtime.choose(false);fs::path base=existing(source,selected);
        if(base.empty()){
            char pattern[]="/tmp/pmu-source-dispatch.XXXXXX";const auto made=mkdtemp(pattern);need(made!=nullptr,"Cannot create private startup transport");scratch=made;
            const auto result=pmu::runIncomingWorker(selected.worker,scratch,{{"operation","apply"},{"intent","prepare-runtime"},{"source",source.string()},{"helper",helper.string()},{"generation",selected.generation.id},{"source_identity",selected.identity}});
            need(result.at("generation")==selected.generation.id&&result.at("source_identity")==selected.identity,"Worker returned a different manager identity");base=result.at("session").get<std::string>();folder(base);
        }
        need(runtime.choose(false,selected.identity).generation.id==selected.generation.id,"Manager source changed before opening");if(mode=="open")open(base);if(!scratch.empty())fs::remove_all(scratch);std::cout<<base.string()<<'\n';return 0;
    }catch(const std::exception& error){if(!scratch.empty()){std::error_code ignored;fs::remove_all(scratch,ignored);}std::fprintf(stderr,"Plugin manager startup: %.2048s\n",error.what());return 1;}
}
