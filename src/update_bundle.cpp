#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "update_bundle.hpp"
#include "checksum.hpp"
#include "metadata.hpp"
#include "crypto.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr const char* envelopeName="PMU-UPDATE.json";
constexpr uint64_t fileLimit=64ULL*1024*1024,totalLimit=128ULL*1024*1024;
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
struct stat information(int fd){struct stat value{};need(fstat(fd,&value)==0,"Cannot inspect update bundle");return value;}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool unchanged(const struct stat& a,const struct stat& b){return same(a,b)&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;}
std::string descriptor(int fd){return "/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);}
Fd folder(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path,"Unsafe update staging path");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect update staging path");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));need(next.value>=0,"Update staging path is missing or linked");current=std::move(next);}return current;
}
struct Archive {
    archive* value=nullptr;
    explicit Archive(int fd){
        need(lseek(fd,0,SEEK_SET)==0,"Cannot rewind update ZIP");value=archive_read_new();need(value!=nullptr,"Cannot allocate update ZIP reader");
        if(archive_read_support_format_zip(value)!=ARCHIVE_OK||archive_read_open_fd(value,fd,65536)!=ARCHIVE_OK){archive_read_free(value);value=nullptr;throw std::runtime_error("Cannot open update ZIP");}
    }
    ~Archive(){if(value)archive_read_free(value);}
    Archive(const Archive&)=delete;
    Archive& operator=(const Archive&)=delete;
};
struct Header {std::string name;bool directory;uint64_t size;};
Header header(archive_entry* entry){
    const char* raw=archive_entry_pathname(entry);need(raw!=nullptr,"Missing update ZIP name");std::string name=raw;const bool directory=archive_entry_filetype(entry)==AE_IFDIR;
    if(directory&&!name.empty()&&name.back()=='/')name.pop_back();
    GenerationSpec probe{"path-check","universal",{{name,std::string(64,'0')}}};(void)generationSpecToJson(probe);
    need(archive_entry_filetype(entry)==AE_IFREG||directory,"Update ZIP contains a special file");need(!archive_entry_symlink(entry)&&!archive_entry_hardlink(entry)&&!archive_entry_is_encrypted(entry),"Update ZIP contains links or encryption");
    const auto size=archive_entry_size(entry);need(archive_entry_size_is_set(entry)&&size>=0&&uint64_t(size)<=fileLimit&&(!directory||!size),"Unsupported update ZIP entry size");return {name,directory,uint64_t(size)};
}
std::string contents(archive* input,uint64_t bytes,size_t maximum){
    need(bytes<=maximum,"Oversized update metadata");std::string result;std::array<char,8192> buffer{};la_ssize_t count;
    while((count=archive_read_data(input,buffer.data(),buffer.size()))>0){need(result.size()+size_t(count)<=bytes,"Update metadata grew during extraction");result.append(buffer.data(),size_t(count));}
    need(count==0&&result.size()==bytes,"Incomplete update metadata");return result;
}
GenerationSpec specification(const Json& value){
    need(value.is_object()&&value.size()==4&&value.value("protocol",0)==1&&value.value("product",std::string())=="plugin-manager-usb"&&value.contains("generation")&&value.contains("authorization"),"Unsupported update bundle protocol");auto spec=generationSpecFromJson(value.at("generation"));need(value.at("generation")==generationSpecToJson(spec),"Unsupported update envelope fields; place future metadata in inventoried files");
    need(!spec.files.count(envelopeName),"Update envelope cannot inventory itself");
    for(const auto& arch:{std::string("armv7"),std::string("aarch64")}){
        if(spec.architecture=="universal"||spec.architecture==arch)need(spec.entrypoints.count("update_worker_"+arch),"Update bundle lacks a native migration worker");
    }
    return spec;
}
struct Staging {
    fs::path path;
    Fd parent,root;
    struct stat identity{};
    std::map<std::string,Fd> directories;
    std::map<std::string,struct stat> files;
    bool retained=false;
    explicit Staging(fs::path destination):path(std::move(destination)),parent(folder(path.parent_path())){
        need(path.is_absolute()&&path.lexically_normal()==path&&!path.filename().empty(),"Invalid update destination");const auto owner=information(parent.value);need(owner.st_uid==geteuid()&&!(owner.st_mode&0022),"Unsafe update scratch folder");
        need(mkdirat(parent.value,path.filename().c_str(),0700)==0,"Update destination already exists or is unavailable");
        need(fstatat(parent.value,path.filename().c_str(),&identity,AT_SYMLINK_NOFOLLOW)==0,"Cannot identify private update staging");
        try{
            root=Fd(openat(parent.value,path.filename().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(root.value>=0,"Cannot open private update staging");const auto pinned=information(root.value);need(same(identity,pinned)&&pinned.st_uid==geteuid()&&!(pinned.st_mode&0022),"Unsafe private update staging");directories.emplace("",Fd(dup(root.value)));need(directories.at("").value>=0,"Cannot retain private update staging");
        }catch(...){struct stat current{};if(fstatat(parent.value,path.filename().c_str(),&current,AT_SYMLINK_NOFOLLOW)==0&&same(current,identity))unlinkat(parent.value,path.filename().c_str(),AT_REMOVEDIR);throw;}
    }
    void check(){
        auto current=folder(path);need(same(information(current.value),identity),"Update staging was removed or replaced");
        for(const auto& item:directories){if(item.first.empty())continue;const fs::path relative(item.first);struct stat named{};const auto parent=directories.find(relative.parent_path().generic_string());need(parent!=directories.end()&&fstatat(parent->second.value,relative.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&same(named,information(item.second.value)),"Update staging folder changed");}
        for(const auto& item:files){const fs::path relative(item.first);struct stat named{};const auto parent=directories.find(relative.parent_path().generic_string());need(parent!=directories.end()&&fstatat(parent->second.value,relative.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&same(named,item.second),"Update staging file changed");}
    }
    int directory(const fs::path& path){
        std::string prefix;int current=root.value;
        for(const auto& part:path){prefix+=(prefix.empty()?"":"/")+part.string();auto found=directories.find(prefix);if(found==directories.end()){
                need(mkdirat(current,part.c_str(),0700)==0,"Cannot create private update folder");Fd next(openat(current,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(next.value>=0,"Cannot pin private update folder");found=directories.emplace(prefix,std::move(next)).first;
            }current=found->second.value;}return current;
    }
    Fd create(const std::string& name){
        check();const fs::path relative(name);const int parent=directory(relative.parent_path());Fd output(openat(parent,relative.filename().c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Cannot create staged update file");files.emplace(name,information(output.value));return output;
    }
    ~Staging(){
        if(retained)return;
        for(const auto& item:files){const fs::path relative(item.first);auto parent=directories.find(relative.parent_path().generic_string());struct stat current{};
            if(parent!=directories.end()&&fstatat(parent->second.value,relative.filename().c_str(),&current,AT_SYMLINK_NOFOLLOW)==0&&same(current,item.second))unlinkat(parent->second.value,relative.filename().c_str(),0);
        }
        for(auto it=directories.rbegin();it!=directories.rend();++it){if(it->first.empty())continue;const fs::path relative(it->first);auto parent=directories.find(relative.parent_path().generic_string());struct stat current{};
            struct stat pinned{};if(parent!=directories.end()&&fstat(it->second.value,&pinned)==0&&fstatat(parent->second.value,relative.filename().c_str(),&current,AT_SYMLINK_NOFOLLOW)==0&&same(current,pinned))unlinkat(parent->second.value,relative.filename().c_str(),AT_REMOVEDIR);
        }
        struct stat current{};if(fstatat(parent.value,path.filename().c_str(),&current,AT_SYMLINK_NOFOLLOW)==0&&same(current,identity))unlinkat(parent.value,path.filename().c_str(),AT_REMOVEDIR);
    }
};
void writeAll(int fd,const char* data,size_t bytes){size_t at=0;while(at<bytes){const auto n=write(fd,data+at,bytes-at);if(n<0&&errno==EINTR)continue;need(n>0,"Cannot write staged update file");at+=size_t(n);}}
}
Json updateEnvelope(const GenerationSpec& spec,const std::string& authorization){Json result={{"protocol",1},{"product","plugin-manager-usb"},{"generation",generationSpecToJson(spec)},{"authorization",authorization}};(void)specification(result);return result;}
UpdateBundle unpackUpdateBundle(const fs::path& zip,const std::string& sha,uint64_t bytes,const fs::path& destination,const std::string& architecture,const std::string& version,const std::string& publisherKey,const UpdateProgress& progress){
    need(bytes>0&&bytes<=totalLimit&&sha.size()==64&&sha.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid expected update ZIP identity");auto zipParent=folder(zip.parent_path());Fd input(openat(zipParent.value,zip.filename().c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));need(input.value>=0,"Cannot pin update ZIP");const auto before=information(input.value);
    need(S_ISREG(before.st_mode)&&before.st_nlink==1&&before.st_uid==geteuid()&&!(before.st_mode&0022)&&before.st_size>=0&&uint64_t(before.st_size)==bytes,"Unsafe or incomplete update ZIP");need(fileSha256(descriptor(input.value))==sha&&unchanged(before,information(input.value)),"Update ZIP checksum mismatch");
    std::string text;std::set<std::string> names;std::map<std::string,Header> entries;uint64_t total=0;
    {Archive archive(input.value);archive_entry* entry=nullptr;int status;
        while((status=archive_read_next_header(archive.value,&entry))==ARCHIVE_OK){auto value=header(entry);need(names.insert(value.name).second&&names.size()<=512,"Duplicate or excessive update ZIP entries");total+=value.size;need(total<=totalLimit,"Expanded update ZIP exceeds its limit");entries.emplace(value.name,value);
            if(value.name==envelopeName){need(!value.directory,"Update envelope is a directory");text=contents(archive.value,value.size,128*1024);}else need(archive_read_data_skip(archive.value)==ARCHIVE_OK,"Cannot inspect update ZIP");
        }need(status==ARCHIVE_EOF&&!text.empty(),"Update ZIP has no complete v1 envelope");
    }
    const auto envelope=parseMetadata(text);const auto spec=specification(envelope);need(spec.version==version,"Update envelope version differs from the selected release");need((architecture=="armv7"||architecture=="aarch64")&&(spec.architecture==architecture||spec.architecture=="universal"),"Update bundle has the wrong device architecture");verifyAuthorization(publisherKey,"update",sha256Text(generationSpecToJson(spec).dump()+'\n'),envelope.at("authorization").get<std::string>());
    std::set<std::string> directories;for(const auto& file:spec.files){auto found=entries.find(file.first);need(found!=entries.end()&&!found->second.directory,"Update envelope lists a missing file");for(auto parent=fs::path(file.first).parent_path();!parent.empty();parent=parent.parent_path())directories.insert(parent.generic_string());}
    for(const auto& entry:entries)need(entry.second.directory?directories.count(entry.first):entry.first==envelopeName||spec.files.count(entry.first),"Update ZIP contains uninventoried content");
    Staging output(destination);uint64_t copied=0;
    {Archive archive(input.value);archive_entry* entry=nullptr;int status;
        while((status=archive_read_next_header(archive.value,&entry))==ARCHIVE_OK){const auto value=header(entry);if(value.directory){need(archive_read_data_skip(archive.value)==ARCHIVE_OK,"Cannot skip update directory");continue;}
            auto to=output.create(value.name);std::array<char,65536> buffer{};uint64_t written=0;la_ssize_t count;
            while((count=archive_read_data(archive.value,buffer.data(),buffer.size()))>0){written+=uint64_t(count);need(written<=value.size,"Update file grew during extraction");writeAll(to.value,buffer.data(),size_t(count));}
            need(count==0&&written==value.size&&fsync(to.value)==0,"Cannot finish staged update file");const auto wanted=value.name==envelopeName?std::string():spec.files.at(value.name);if(!wanted.empty())need(fileSha256(descriptor(to.value))==wanted,"Staged update checksum mismatch");copied+=written;if(progress.fraction)progress.fraction(float(copied)/float(total));
        }need(status==ARCHIVE_EOF,"Update ZIP ended unexpectedly");
    }
    need(unchanged(before,information(input.value)),"Update ZIP changed during extraction");output.check();need(fsync(output.root.value)==0,"Cannot finish private update staging");const auto role=spec.entrypoints.at("update_worker_"+architecture);output.retained=true;return {destination,spec,{destination/role,spec.files.at(role),architecture}};
}
}
