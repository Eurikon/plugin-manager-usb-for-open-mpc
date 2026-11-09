#pragma once
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace pmu {
inline bool nativeMpcStopped(){
    for(const auto& entry:std::filesystem::directory_iterator("/proc")){
        const auto name=entry.path().filename().string();
        if(name.empty()||name.find_first_not_of("0123456789")!=std::string::npos)continue;
        std::ifstream input(entry.path()/"comm");std::string command;
        if(std::getline(input,command)&&command=="MPC")return false;
    }
    return true;
}
class NativeCache {
    using Path=std::filesystem::path;
    struct Folder {
        int fd=-1;
        explicit Folder(const Path& path){
            if(!path.is_absolute()||path.lexically_normal()!=path)throw std::runtime_error("Invalid native cache directory");
            fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            for(const auto& part:path.relative_path()){
                const int next=fd<0?-1:openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                if(fd>=0)close(fd);
                fd=next;
                if(fd<0)throw std::runtime_error("Native cache directory is missing or linked");
            }
            struct stat value{};
            if(fstat(fd,&value)||value.st_uid!=0){close(fd);fd=-1;throw std::runtime_error("Native cache directory is not root-owned");}
        }
        ~Folder(){if(fd>=0)close(fd);}
        Folder(const Folder&)=delete;
        Folder& operator=(const Folder&)=delete;
    };
    Path ramParent,native;
    std::function<bool()> stopped;
    static void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static bool memory(int fd){struct statfs value{};need(fstatfs(fd,&value)==0,"Cannot inspect scratch filesystem");return value.f_type==0x01021994;}
    Path nativeDirectory()const{
        need(native.filename()=="filmstrips","Unsupported native cache name");
        const auto path=std::filesystem::canonical(native.parent_path())/native.filename();Folder parent(path.parent_path());
        struct stat value{};
        if(fstatat(parent.fd,"filmstrips",&value,AT_SYMLINK_NOFOLLOW)==0)need(S_ISDIR(value.st_mode)&&value.st_uid==0,"Native cache target is linked or unowned");
        else need(errno==ENOENT,"Cannot inspect native cache target");
        return path;
    }
    Path ramDirectory()const{
        const auto parent=std::filesystem::canonical(ramParent);Folder directory(parent);
        need(memory(directory.fd),"Native filmstrip cache requires RAM-backed storage");
        if(mkdirat(directory.fd,"filmstrips",01777)!=0)need(errno==EEXIST,"Cannot create RAM filmstrip cache");
        const auto result=parent/"filmstrips";Folder cache(result);need(memory(cache.fd),"Native filmstrip cache is not RAM-backed");return result;
    }
    size_t clear(const Path& path)const{
        Folder folder(path);struct stat pinned{};need(fstat(folder.fd,&pinned)==0,"Cannot pin native cache");
        const int scan=openat(folder.fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);need(scan>=0,"Cannot inspect native cache files");
        DIR* entries=fdopendir(scan);if(!entries){close(scan);throw std::runtime_error("Cannot list native cache files");}
        size_t count=0;
        try{
            for(;;){
                errno=0;const auto entry=readdir(entries);if(!entry){need(errno==0,"Cannot finish native cache listing");break;}
                const std::string name=entry->d_name;
                if(name.size()<=9||name.rfind("temp_",0)!=0||name.compare(name.size()-4,4,".img")!=0)continue;
                const auto token=name.substr(5,name.size()-9);if(token.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
                struct stat file{};need(fstatat(folder.fd,name.c_str(),&file,AT_SYMLINK_NOFOLLOW)==0,"Native cache file changed");
                if(!S_ISREG(file.st_mode)||file.st_uid!=0||file.st_nlink!=1)continue;
                need(stopped(),"MPC must stay stopped during native cache cleanup");
                Folder named(path);struct stat current{};need(fstat(named.fd,&current)==0&&current.st_dev==pinned.st_dev&&current.st_ino==pinned.st_ino,"Native cache directory changed");
                need(unlinkat(folder.fd,name.c_str(),0)==0,"Cannot remove inactive native cache image");++count;
            }
        }catch(...){closedir(entries);throw;}
        closedir(entries);return count;
    }
public:
    NativeCache(Path ram="/var/volatile/tmp",Path cache="/var/tmp/filmstrips",std::function<bool()> check=nativeMpcStopped):ramParent(std::move(ram)),native(std::move(cache)),stopped(std::move(check)){}
    static void requireRam(const Path& path){Folder folder(std::filesystem::canonical(path));need(memory(folder.fd),"Manager scratch storage must be RAM-backed");}
    static void serviceMountNamespace(){
        // Existing addins may isolate service mounts; cleanup must see the original cache.
        const int current=open("/proc/self/ns/mnt",O_RDONLY|O_CLOEXEC),system=open("/proc/1/ns/mnt",O_RDONLY|O_CLOEXEC);
        struct stat a{},b{};const bool readable=current>=0&&system>=0&&fstat(current,&a)==0&&fstat(system,&b)==0;
        const bool changed=readable&&(a.st_dev!=b.st_dev||a.st_ino!=b.st_ino);
        const bool entered=readable&&(!changed||setns(system,CLONE_NEWNS)==0);
        if(current>=0)close(current);
        if(system>=0)close(system);
        need(entered,"Cannot inspect the stock native cache mount namespace");
    }
    void validate()const{nativeDirectory();ramDirectory();}
    size_t clean()const{
        need(stopped(),"MPC must be stopped before native cache cleanup");const auto path=nativeDirectory(),ram=ramDirectory();
        size_t count=clear(ram);
        if(std::filesystem::exists(path)){
            Folder first(ram),second(path);struct stat a{},b{};
            need(fstat(first.fd,&a)==0&&fstat(second.fd,&b)==0,"Cannot compare native cache directories");
            if(a.st_dev!=b.st_dev||a.st_ino!=b.st_ino)count+=clear(path);
        }
        need(stopped(),"MPC resumed during native cache cleanup");return count;
    }
    size_t prepare()const{
        const auto count=clean();const auto path=nativeDirectory(),ram=ramDirectory();
        if(!std::filesystem::exists(path)){Folder parent(path.parent_path());need(mkdirat(parent.fd,"filmstrips",01777)==0,"Cannot create native cache mount point");}
        Folder target(path),source(ram);
        if(!memory(target.fd)){
            need(stopped(),"MPC must stay stopped before native cache redirection");
            need(mount(ram.c_str(),path.c_str(),nullptr,MS_BIND,nullptr)==0,"Cannot redirect native filmstrips to RAM");
            Folder redirected(path);struct stat a{},b{};
            need(memory(redirected.fd)&&fstat(redirected.fd,&a)==0&&fstat(source.fd,&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino,"Native cache RAM redirection verification failed");
        }
        need(stopped(),"MPC resumed during native cache preparation");return count;
    }
};
}
