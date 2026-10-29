#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "source_runtime.hpp"
#include "checksum.hpp"
#include "crypto.hpp"
#include "filesystem_identity.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
constexpr const char* markerName=".mpc-manager-id";
constexpr char markerText[]="mpc-usb-manager-v1\n";
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
bool fat(int fd){struct statfs value{};return fstatfs(fd,&value)==0&&(value.f_type==0x2011bab0||value.f_type==0x4d44);}
struct stat info(int fd){struct stat value{};need(fstat(fd,&value)==0,"Cannot inspect manager source");need(value.st_uid==geteuid()&&(!(value.st_mode&0022)||fat(fd)),"Manager source has unsafe ownership or permissions");return value;}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
Fd folder(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path!=path.root_path(),"Invalid manager source path");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot open manager source root");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(next.value>=0,"Manager source is missing or linked");current=std::move(next);}return current;
}
Fd marker(int root){
    Fd result(openat(root,markerName,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));need(result.value>=0,"Manager source marker is missing");const auto status=info(result.value);need(S_ISREG(status.st_mode)&&status.st_nlink==1&&status.st_size==sizeof(markerText)-1,"Invalid manager source marker");std::array<char,sizeof(markerText)> text{};need(pread(result.value,text.data(),text.size(),0)==ssize_t(sizeof(markerText)-1)&&std::string(text.data(),sizeof(markerText)-1)==markerText,"Invalid manager source marker content");return result;
}
std::string identity(const struct stat& root,const struct stat& marker){return std::to_string(uintmax_t(root.st_dev))+":"+std::to_string(uintmax_t(root.st_ino))+":"+std::to_string(uintmax_t(marker.st_dev))+":"+std::to_string(uintmax_t(marker.st_ino));}
}
struct SourceRuntime::Impl {
    fs::path source;
    std::string architecture;
    Fd root,markerFile;
    struct stat rootIdentity{},markerIdentity{};
    std::string mount;
    VersionStore store;
    Impl(fs::path path,std::string arch,std::string publisher):source(std::move(path)),architecture(std::move(arch)),root(folder(source)),markerFile(marker(root.value)),rootIdentity(info(root.value)),markerIdentity(info(markerFile.value)),mount(mountIdentity(root.value)),store(source/"autorun/runtime/.pmu-versions",geteuid(),[](const auto& path){return fileSha256(path);},{},false,[publisher](const auto& id,const auto&,const auto& proof){verifyAuthorization(publisher,"runtime",id,proof);}){
        need(architecture=="armv7"||architecture=="aarch64","Unsupported manager source architecture");
    }
    std::string identity()const{return pmu::identity(rootIdentity,markerIdentity)+":"+mount;}
    void check(const std::string& expected={})const{
        need(expected.empty()||expected==identity(),"Manager source was removed or replaced");auto current=folder(source);auto currentMarker=marker(current.value);need(same(rootIdentity,info(current.value))&&same(markerIdentity,info(currentMarker.value))&&mountIdentity(current.value)==mount,"Manager source was removed or replaced");
    }
};
SourceRuntime::SourceRuntime(fs::path source,std::string architecture,std::string publisher):impl(std::make_unique<Impl>(std::move(source),std::move(architecture),std::move(publisher))){}
SourceRuntime::~SourceRuntime()=default;
RuntimeChoice SourceRuntime::choose(bool opening,const std::string& expected){
    impl->check(expected);std::optional<Generation> selected;
    if(opening)selected=impl->store.boot();
    else{try{selected=impl->store.current();}catch(const std::exception&){selected=impl->store.boot();}if(!selected)selected=impl->store.boot();}
    need(selected.has_value(),"No complete manager version is available on this source");need(selected->spec.architecture==impl->architecture||selected->spec.architecture=="universal","Manager version does not support this device generation");const auto role=selected->spec.entrypoints.find("runtime_worker_"+impl->architecture);need(role!=selected->spec.entrypoints.end(),"Manager version has no native session worker");impl->check(expected);return {*selected,{selected->directory/role->second,selected->spec.files.at(role->second),impl->architecture},impl->identity()};
}
void SourceRuntime::confirm(const std::string& generation,const std::string& expected){
    need(!expected.empty(),"Missing manager startup source identity");impl->check(expected);const auto selected=impl->store.current();need(selected&&selected->id==generation,"Stale manager startup confirmation");const auto role=selected->spec.entrypoints.find("manager_"+impl->architecture);need(role!=selected->spec.entrypoints.end(),"Manager generation lacks its GUI startup role");Fd running(open("/proc/self/exe",O_RDONLY|O_CLOEXEC));need(running.value>=0,"Cannot identify the running manager");
    need(fileSha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(running.value))==selected->spec.files.at(role->second),"Running manager does not match the selected version");impl->store.confirm(generation);impl->check(expected);
}
RuntimeChoice SourceRuntime::recover(const std::string& failed,const std::string& expected){need(!expected.empty(),"Missing failed-start source identity");impl->check(expected);impl->store.reject(failed);return choose(false,expected);}
std::string SourceRuntime::identity()const{impl->check();return impl->identity();}
}
