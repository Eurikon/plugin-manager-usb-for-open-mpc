#pragma once
#include "checksum.hpp"
#include "filesystem_identity.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <vector>

namespace pmu {
class SettingsBackups {
    using Path=std::filesystem::path;
    struct Descriptor {int value=-1;~Descriptor(){if(value>=0)close(value);}} parentFd,storeFd;
    Path settings,parent,store;
    struct stat parentIdentity{};
    struct stat storeIdentity{};
    std::string mount;
    std::set<std::string> before;
    static constexpr const char* owner="Plugin Manager USB settings backups v1\n";
    static void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec;}
    static bool regular(const Path& path,struct stat& value){return lstat(path.c_str(),&value)==0&&S_ISREG(value.st_mode)&&value.st_nlink==1&&value.st_size>=0&&value.st_size<=16*1024*1024&&value.st_uid==geteuid();}
    void checkParent()const{
        struct stat value{};need(lstat(parent.c_str(),&value)==0&&S_ISDIR(value.st_mode)&&value.st_dev==parentIdentity.st_dev&&value.st_ino==parentIdentity.st_ino,"Settings backup directory changed");
        Descriptor current{open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
        need(current.value>=0&&mountIdentity(current.value)==mount,"Settings backup mount changed");
    }
    bool sourceName(const std::string& name)const{
        const auto prefix=settings.filename().string();
        if(name.rfind(prefix+".pmu-",0)==0){const auto token=name.substr(prefix.size()+5);return token.size()==6&&token.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")==std::string::npos;}
        if(name.rfind(prefix+".bak-",0)!=0||name.size()<=prefix.size()+20)return false;
        const auto stamp=name.substr(name.size()-15);return stamp[8]=='-'&&stamp.substr(0,8).find_first_not_of("0123456789")==std::string::npos&&stamp.substr(9).find_first_not_of("0123456789")==std::string::npos;
    }
    void openStore(){
        checkParent();
        if(storeFd.value>=0){checkStore();return;}
        if(mkdir(store.c_str(),0700)==0){
            Descriptor marker{open((store/".owner").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600)};
            const std::string text=owner;need(marker.value>=0&&::write(marker.value,text.data(),text.size())==ssize_t(text.size())&&fsync(marker.value)==0,"Cannot identify settings backup folder");
        }else need(errno==EEXIST,"Cannot create settings backup folder");
        struct stat directory{},marker{};need(lstat(store.c_str(),&directory)==0&&S_ISDIR(directory.st_mode)&&directory.st_uid==geteuid()&&(directory.st_mode&0077)==0,"Settings backup folder is not owned by this manager");
        need(regular(store/".owner",marker)&&marker.st_size==ssize_t(std::string(owner).size()),"Settings backup ownership marker is invalid");
        std::ifstream input(store/".owner");const std::string text{std::istreambuf_iterator<char>(input),{}};need(text==owner,"Settings backup ownership marker is invalid");
        storeFd.value=open(store.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(storeFd.value>=0&&fstat(storeFd.value,&storeIdentity)==0,"Cannot pin settings backup folder");checkStore();
    }
    void checkStore()const{
        checkParent();struct stat value{};need(lstat(store.c_str(),&value)==0&&S_ISDIR(value.st_mode)&&value.st_dev==storeIdentity.st_dev&&value.st_ino==storeIdentity.st_ino,"Settings backup folder changed");
    }
    void sync()const{
        Descriptor directory{open(store.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
        need(directory.value>=0&&fsync(directory.value)==0&&fsync(parentFd.value)==0,"Cannot sync settings backup folder");
    }
public:
    explicit SettingsBackups(const Path& path):settings(path),parent(path.parent_path()),store(parent/(path.filename().string()+".pmu-backups")){
        need(path.is_absolute()&&std::filesystem::canonical(parent)==parent,"Settings backup path must be an ordinary absolute directory");
        parentFd.value=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(parentFd.value>=0&&fstat(parentFd.value,&parentIdentity)==0,"Cannot pin settings backup directory");mount=mountIdentity(parentFd.value);
        size_t count=0;for(const auto& entry:std::filesystem::directory_iterator(parent)){need(++count<=8192,"Too many files in settings directory");if(sourceName(entry.path().filename().string()))before.insert(entry.path().filename().string());}
    }
    SettingsBackups(const SettingsBackups&)=delete;
    SettingsBackups& operator=(const SettingsBackups&)=delete;
    void finish(){
        checkParent();std::vector<Path> sources;
        size_t count=0;for(const auto& entry:std::filesystem::directory_iterator(parent)){
            need(++count<=8192,"Too many files in settings directory");const auto name=entry.path().filename().string();struct stat value{};
            if(sourceName(name)&&!before.count(name)&&regular(entry.path(),value))sources.push_back(entry.path());
        }
        if(sources.empty())return;
        openStore();std::set<Path> current;
        for(const auto& source:sources){
            checkStore();struct stat beforeHash{},afterHash{};need(regular(source,beforeHash),"New settings backup changed before archiving");const auto hash=fileSha256(source,std::chrono::seconds(5));
            need(regular(source,afterHash)&&same(beforeHash,afterHash),"New settings backup changed during checksum");
            const auto now=std::time(nullptr);std::tm time{};gmtime_r(&now,&time);char stamp[32];std::strftime(stamp,sizeof stamp,"%d-%m-%Y-%H%M%S",&time);
            const auto pattern=(store/(std::string("backup-")+stamp+"-"+hash+"-XXXXXX")).string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
            Descriptor slot{mkstemp(name.data())};need(slot.value>=0,"Cannot reserve settings backup entry");const Path destination=name.data();
            try{checkStore();std::filesystem::rename(source,destination);need(chmod(destination.c_str(),0600)==0,"Cannot protect settings backup");Descriptor file{open(destination.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC)};need(file.value>=0&&fsync(file.value)==0,"Cannot sync settings backup");current.insert(destination);}
            catch(...){struct stat value{};if(lstat(destination.c_str(),&value)==0&&value.st_size==0)unlink(destination.c_str());throw;}
        }
        struct Entry {Path path;struct stat identity;std::string hash;};std::vector<Entry> entries;
        const std::regex format("backup-[0-9]{2}-[0-9]{2}-[0-9]{4}-[0-9]{6}-([0-9a-f]{64})-[A-Za-z0-9]{6}");count=0;
        for(const auto& entry:std::filesystem::directory_iterator(store)){
            need(++count<=256,"Settings backup folder needs manual review");std::smatch match;const auto name=entry.path().filename().string();struct stat value{};
            if(std::regex_match(name,match,format)&&regular(entry.path(),value))entries.push_back({entry.path(),value,match[1]});
        }
        std::sort(entries.begin(),entries.end(),[&](const Entry& a,const Entry& b){if(current.count(a.path)!=current.count(b.path))return current.count(a.path)>current.count(b.path);if(a.identity.st_mtim.tv_sec!=b.identity.st_mtim.tv_sec)return a.identity.st_mtim.tv_sec>b.identity.st_mtim.tv_sec;if(a.identity.st_mtim.tv_nsec!=b.identity.st_mtim.tv_nsec)return a.identity.st_mtim.tv_nsec>b.identity.st_mtim.tv_nsec;return a.path>b.path;});
        for(size_t i=10;i<entries.size();++i){const auto& entry=entries[i];checkStore();if(fileSha256(entry.path,std::chrono::seconds(5))!=entry.hash)continue;struct stat value{};need(regular(entry.path,value)&&same(value,entry.identity),"Settings backup changed before pruning");need(unlink(entry.path.c_str())==0,"Cannot prune settings backup");}
        sync();
    }
};
}
