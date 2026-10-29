#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "version_store.hpp"
#include "metadata.hpp"
#include "filesystem_identity.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr size_t maximumFiles=256,maximumFile=64*1024*1024,maximumPayload=128*1024*1024;
constexpr const char* receipt="generation.json";
constexpr const char* authorizationFile="authorization.json";
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
struct CloseDirectory {void operator()(DIR* value)const{if(value)closedir(value);}};
std::unique_ptr<DIR,CloseDirectory> listing(int directory){
    const int fd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(fd>=0,"Cannot open generation inventory");
    DIR* entries=fdopendir(fd);if(!entries)close(fd);need(entries!=nullptr,"Cannot list generation inventory");return std::unique_ptr<DIR,CloseDirectory>(entries);
}
dirent* nextEntry(DIR* directory){errno=0;auto* entry=readdir(directory);if(!entry&&errno)throw std::runtime_error("Cannot read generation inventory: "+std::string(std::strerror(errno)));return entry;}
bool hex(const std::string& text,size_t count=64){return text.size()==count&&text.find_first_not_of("0123456789abcdef")==std::string::npos;}
std::array<unsigned long,3> releaseVersion(const std::string& value){
    std::array<unsigned long,3> result{};std::istringstream input(value);std::string part;for(size_t i=0;i<3;++i){need(bool(std::getline(input,part,'.'))&&!part.empty()&&part.size()<=9&&part.find_first_not_of("0123456789")==std::string::npos&&(part.size()==1||part.front()!='0'),"Invalid stable release version");result[i]=std::stoul(part);}need(input.eof(),"Invalid stable release version");return result;
}
bool relative(const std::string& text){
    const fs::path path(text);
    if(text.empty()||text.size()>240||path.is_absolute()||path.generic_string()!=text||path.lexically_normal()!=path)return false;
    for(const auto& part:path){const auto name=part.string();if(name.empty()||name=="."||name==".."||name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.- ")!=std::string::npos)return false;}
    return true;
}
Fd descend(int parent,const fs::path& path,bool create=false,bool optional=false){
    Fd current(dup(parent));need(current.value>=0,"Cannot retain version directory");
    for(const auto& part:path){
        if(part.empty()||part==".")continue;
        need(part!=".."&&!part.is_absolute(),"Unsafe version directory");
        if(create){need(mkdirat(current.value,part.c_str(),0700)==0||errno==EEXIST,"Cannot create version directory");need(fsync(current.value)==0,"Cannot sync version directory");}
        Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        if(next.value<0&&optional&&errno==ENOENT)return Fd{};
        need(next.value>=0,"Version directory is missing or contains a link");current=std::move(next);
    }
    return current;
}
Fd tree(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos,"Version store needs a normalized absolute path");
    Fd root(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(root.value>=0,"Cannot inspect version root");
    return descend(root.value,path.relative_path());
}
struct stat info(int fd){struct stat value{};need(fstat(fd,&value)==0,"Cannot inspect version file");return value;}
bool fat(int fd){struct statfs value{};return fstatfs(fd,&value)==0&&(value.f_type==0x2011bab0||value.f_type==0x4d44);}
bool present(int dir,const std::string& name){struct stat value{};if(fstatat(dir,name.c_str(),&value,AT_SYMLINK_NOFOLLOW)==0)return true;need(errno==ENOENT,"Cannot inspect version entry");return false;}
void sync(int fd){need(fsync(fd)==0,"Cannot sync version storage");}
std::string randomName(){
    Fd random(open("/dev/urandom",O_RDONLY|O_CLOEXEC));need(random.value>=0,"Cannot identify version staging");
    std::array<unsigned char,16> bytes{};size_t at=0;
    while(at<bytes.size()){const auto n=read(random.value,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;need(n>0,"Cannot identify version staging");at+=size_t(n);}
    std::string result;const char* digits="0123456789abcdef";for(auto byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
struct Selection {
    std::string active,previous;
    bool pending=false,attempted=false;
};
Json selectionJson(const Selection& s){return {{"schema",1},{"active",s.active},{"previous",s.previous},{"pending",s.pending},{"attempted",s.attempted}};}
Selection selection(const Json& value){
    need(value.is_object()&&value.value("schema",0)==1,"Unsupported version selection");
    Selection s{value.at("active").get<std::string>(),value.at("previous").get<std::string>(),value.at("pending").get<bool>(),value.at("attempted").get<bool>()};
    need(hex(s.active)&&(s.previous.empty()||hex(s.previous))&&s.active!=s.previous&&(!s.attempted||s.pending),"Invalid version selection");return s;
}
void validate(const GenerationSpec& spec){
    need(!spec.version.empty()&&spec.version.size()<=80&&spec.version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==std::string::npos,"Invalid generation version");
    need(spec.architecture=="armv7"||spec.architecture=="aarch64"||spec.architecture=="universal","Invalid generation architecture");
    need(!spec.files.empty()&&spec.files.size()<=maximumFiles,"Invalid generation inventory");
    for(const auto& item:spec.files){
        need(relative(item.first)&&*fs::path(item.first).begin()!=receipt&&*fs::path(item.first).begin()!=authorizationFile&&hex(item.second),"Invalid generation file");
        for(auto parent=fs::path(item.first).parent_path();!parent.empty();parent=parent.parent_path())need(!spec.files.count(parent.generic_string()),"Generation file conflicts with a directory");
    }
    need(spec.entrypoints.size()<=32,"Too many generation entrypoints");
    for(const auto& item:spec.entrypoints)need(!item.first.empty()&&item.first.size()<=80&&item.first.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==std::string::npos&&spec.files.count(item.second),"Invalid generation entrypoint");
}
GenerationSpec specification(const Json& value){
    need(value.is_object()&&value.value("schema",0)==1,"Unsupported generation inventory");
    GenerationSpec spec{value.at("version").get<std::string>(),value.at("architecture").get<std::string>(),value.at("files").get<std::map<std::string,std::string>>()};if(value.contains("entrypoints"))spec.entrypoints=value.at("entrypoints").get<std::map<std::string,std::string>>();validate(spec);return spec;
}
Json specificationJson(const GenerationSpec& s){return {{"schema",1},{"version",s.version},{"architecture",s.architecture},{"files",s.files},{"entrypoints",s.entrypoints}};}
}

struct VersionStore::Impl {
    fs::path path;
    uid_t owner;
    Hash hash;
    Checkpoint checkpoint;
    Trust trust;
    Fd root;
    struct stat identity{};
    std::string mount;
    std::string profile;
    Impl(fs::path location,uid_t user,Hash digest,Checkpoint hook,bool create,Trust approve,std::string selectedProfile):path(std::move(location)),owner(user),hash(std::move(digest)),checkpoint(std::move(hook)),trust(std::move(approve)),profile(std::move(selectedProfile)){
        need(profile.empty()||profile=="armv7"||profile=="aarch64","Unsupported version selection profile");
        need(bool(hash),"Version store requires a hash provider");
        need(path.is_absolute()&&path.lexically_normal()==path&&path!=path.root_path(),"Invalid version store root");
        if(create){auto parent=tree(path.parent_path());need(mkdirat(parent.value,path.filename().c_str(),0700)==0||errno==EEXIST,"Cannot create version store");sync(parent.value);}
        root=tree(path);checkOwner(root.value,true);identity=info(root.value);mount=mountIdentity(root.value);
    }
    void checkOwner(int fd,bool directory=false){
        const auto value=info(fd);need(value.st_uid==owner&&(directory?S_ISDIR(value.st_mode):(S_ISREG(value.st_mode)&&value.st_nlink==1)),"Unowned or unsafe version entry");
        need(!(value.st_mode&0022)||fat(fd),"Version entry is writable by another user");
    }
    void recheck(){auto current=tree(path);const auto value=info(current.value);need(value.st_dev==identity.st_dev&&value.st_ino==identity.st_ino&&mountIdentity(current.value)==mount,"Version storage was removed or replaced");checkOwner(current.value,true);}
    Fd lock(){
        recheck();Fd result(openat(root.value,"update.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600));need(result.value>=0,"Cannot open version lock");checkOwner(result.value);need(flock(result.value,LOCK_EX|LOCK_NB)==0,"Another version operation is running");return result;
    }
    void point(const std::string& name){if(checkpoint)checkpoint(name);recheck();}
    Fd file(int directory,const std::string& name){
        auto parent=descend(directory,fs::path(name).parent_path());Fd result(openat(parent.value,fs::path(name).filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(result.value>=0,"Missing or linked generation file");checkOwner(result.value);return result;
    }
    std::string digest(int fd){
        const auto before=info(fd);const auto value=hash("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd));const auto after=info(fd);
        need(hex(value)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"Generation changed while hashing");return value;
    }
    std::string readAt(int directory,const std::string& name,size_t limit){
        auto input=file(directory,name);const auto before=info(input.value);need(before.st_size>=0&&uint64_t(before.st_size)<=limit,"Oversized version metadata");
        std::string text;std::array<char,4096> buffer{};
        for(;;){const auto n=read(input.value,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;need(n>=0,"Cannot read version metadata");if(!n)break;need(text.size()+size_t(n)<=limit,"Version metadata grew while reading");text.append(buffer.data(),size_t(n));}
        const auto after=info(input.value);need(text.size()==size_t(before.st_size)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"Version metadata changed while reading");return text;
    }
    void save(int directory,const std::string& name,const std::string& bytes){
        auto parent=descend(directory,fs::path(name).parent_path(),true);
        Fd output(openat(parent.value,fs::path(name).filename().c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Cannot create generation file");
        size_t at=0;while(at<bytes.size()){const auto n=write(output.value,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;need(n>0,"Cannot write generation file");at+=size_t(n);}sync(output.value);sync(parent.value);
    }
    std::string recordName(const std::string& name)const{return profile.empty()?name:name.substr(0,name.size()-5)+"."+profile+".json";}
    void record(const std::string& logicalName,const Selection& value){
        const auto name=recordName(logicalName);
        if(present(root.value,name)){auto previous=file(root.value,name);(void)previous;}
        const auto fresh=".record-"+randomName();
        try{save(root.value,fresh,selectionJson(value).dump()+'\n');point("selection-synced:"+logicalName);need(renameat(root.value,fresh.c_str(),root.value,name.c_str())==0,"Cannot activate generation");point("selection-published:"+logicalName);sync(root.value);point("selection-durable:"+logicalName);}
        catch(...){unlinkat(root.value,fresh.c_str(),0);throw;}
    }
    std::optional<Selection> namedState(const std::string& name){if(!present(root.value,name))return {};return selection(parseMetadata(readAt(root.value,name,1024)));}
    std::optional<Selection> state(const std::string& name){return namedState(recordName(name));}
    void inventory(int directory,const GenerationSpec& spec,const std::string& prefix={}){
        auto entries=listing(directory);
        while(auto* entry=nextEntry(entries.get())){
            const std::string leaf=entry->d_name;if(leaf=="."||leaf=="..")continue;
            const auto name=prefix+leaf;struct stat value{};need(fstatat(directory,leaf.c_str(),&value,AT_SYMLINK_NOFOLLOW)==0,"Generation changed during inspection");
            if(S_ISDIR(value.st_mode)){
                const auto first=spec.files.lower_bound(name+'/');need(first!=spec.files.end()&&first->first.rfind(name+'/',0)==0,"Generation contains an unowned directory");
                auto child=descend(directory,leaf);checkOwner(child.value,true);inventory(child.value,spec,name+'/');
            }else need(S_ISREG(value.st_mode)&&value.st_nlink==1&&(spec.files.count(name)||(prefix.empty()&&(name==receipt||name==authorizationFile))),"Generation contains an unowned or unsafe file");
        }
    }
    Generation generation(const std::string& id,bool pending=false){
        need(hex(id),"Invalid generation identifier");auto folder=descend(root.value,id);checkOwner(folder.value,true);
        auto input=file(folder.value,receipt);need(digest(input.value)==id,"Generation inventory checksum mismatch");
        const auto spec=specification(parseMetadata(readAt(folder.value,receipt,128*1024)));const auto authorization=present(folder.value,authorizationFile)?readAt(folder.value,authorizationFile,8192):std::string();if(trust)trust(id,spec,authorization);inventory(folder.value,spec);uint64_t size=0;
        for(const auto& item:spec.files){auto payload=file(folder.value,item.first);const auto value=info(payload.value);need(value.st_size>=0&&uint64_t(value.st_size)<=maximumFile,"Invalid generation file size");size+=uint64_t(value.st_size);need(size<=maximumPayload,"Generation exceeds its size limit");need(digest(payload.value)==item.second,"Generation file checksum mismatch");}
        return {id,path/id,spec,pending,authorization};
    }
    std::vector<std::string> names(int directory){
        auto entries=listing(directory);
        std::vector<std::string> result;while(auto* entry=nextEntry(entries.get()))if(std::string(entry->d_name)!="."&&std::string(entry->d_name)!=".."){need(result.size()<1024,"Version storage inventory exceeds its bound");result.emplace_back(entry->d_name);}return result;
    }
    void removeGeneration(const std::string& name,const GenerationSpec& spec){
        auto folder=descend(root.value,name);checkOwner(folder.value,true);inventory(folder.value,spec);std::set<std::string> directories;
        for(const auto& item:spec.files)for(auto p=fs::path(item.first).parent_path();!p.empty();p=p.parent_path())directories.insert(p.generic_string());
        for(const auto& item:spec.files){auto parent=descend(folder.value,fs::path(item.first).parent_path(),false,true);if(parent.value<0||!present(parent.value,fs::path(item.first).filename().string()))continue;auto ownedFile=file(parent.value,fs::path(item.first).filename().string());need(unlinkat(parent.value,fs::path(item.first).filename().c_str(),0)==0,"Cannot retire generation file");sync(parent.value);point("retirement-file-durable");}
        std::vector<std::string> ordered(directories.begin(),directories.end());std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
        for(const auto& directory:ordered){auto parent=descend(folder.value,fs::path(directory).parent_path(),false,true);if(parent.value<0||!present(parent.value,fs::path(directory).filename().string()))continue;need(unlinkat(parent.value,fs::path(directory).filename().c_str(),AT_REMOVEDIR)==0,"Generation contains unowned files; preserved");sync(parent.value);}
        if(present(folder.value,authorizationFile)){auto proof=file(folder.value,authorizationFile);need(info(proof.value).st_size<=8192,"Oversized retired authorization");need(unlinkat(folder.value,authorizationFile,0)==0,"Cannot retire generation authorization");sync(folder.value);point("retirement-authorization-durable");}
        need(unlinkat(folder.value,receipt,0)==0,"Cannot retire generation inventory");
        sync(folder.value);need(unlinkat(root.value,name.c_str(),AT_REMOVEDIR)==0,"Generation contains unowned files; preserved");sync(root.value);
    }
    void recoverStaging(){
        for(const auto& name:names(root.value)){
            recheck();
            if(name.rfind(".record-",0)==0&&hex(name.substr(8),32)){auto metadata=file(root.value,name);need(info(metadata.value).st_size<=1024,"Oversized interrupted selection record");need(unlinkat(root.value,name.c_str(),0)==0,"Cannot clear interrupted selection record");sync(root.value);continue;}
            const bool staged=name.rfind(".stage-",0)==0&&hex(name.substr(7),32);
            const bool retired=name.rfind(".retired-",0)==0&&hex(name.substr(9));
            if(!staged&&!retired)continue;
            auto folder=descend(root.value,name);checkOwner(folder.value,true);const auto entries=names(folder.value);
            if(entries.empty()){need(unlinkat(root.value,name.c_str(),AT_REMOVEDIR)==0,"Cannot clear empty staging directory");sync(root.value);continue;}
            if(entries.size()==1&&entries[0]==receipt){auto metadata=file(folder.value,receipt);need(info(metadata.value).st_size<=128*1024,"Oversized interrupted generation inventory");need(unlinkat(folder.value,receipt,0)==0,"Cannot clear interrupted generation inventory");sync(folder.value);need(unlinkat(root.value,name.c_str(),AT_REMOVEDIR)==0,"Cannot clear interrupted generation directory");sync(root.value);continue;}
            const auto spec=specification(parseMetadata(readAt(folder.value,receipt,128*1024)));if(retired){auto metadata=file(folder.value,receipt);need(digest(metadata.value)==name.substr(9),"Retired generation identity changed");for(const auto& item:spec.files){auto parent=descend(folder.value,fs::path(item.first).parent_path(),false,true);if(parent.value<0||!present(parent.value,fs::path(item.first).filename().string()))continue;auto remaining=file(parent.value,fs::path(item.first).filename().string());need(digest(remaining.value)==item.second,"Modified retired generation file was preserved");}}
            removeGeneration(name,spec);
        }
    }
};

GenerationSpec generationSpecFromJson(const Json& value){return specification(value);}
Json generationSpecToJson(const GenerationSpec& spec){validate(spec);return specificationJson(spec);}
VersionStore::VersionStore(fs::path root,uid_t owner,Hash hash,Checkpoint checkpoint,bool create,Trust trust,std::string profile):impl(std::make_unique<Impl>(std::move(root),owner,std::move(hash),std::move(checkpoint),create,std::move(trust),std::move(profile))){}
VersionStore::~VersionStore()=default;
std::string VersionStore::stage(const fs::path& source,const GenerationSpec& spec,const std::string& authorization){
    validate(spec);need(authorization.size()<=8192,"Oversized generation authorization");auto guard=impl->lock();impl->recoverStaging();auto input=tree(source);const auto temporary=".stage-"+randomName();
    need(mkdirat(impl->root.value,temporary.c_str(),0700)==0,"Cannot stage generation");auto output=descend(impl->root.value,temporary);sync(impl->root.value);
    impl->save(output.value,receipt,specificationJson(spec).dump()+'\n');auto record=impl->file(output.value,receipt);const auto id=impl->digest(record.value);
    if(impl->trust)impl->trust(id,spec,authorization);
    if(present(impl->root.value,id)){impl->generation(id);need(unlinkat(output.value,receipt,0)==0,"Cannot discard duplicate staging receipt");sync(output.value);need(unlinkat(impl->root.value,temporary.c_str(),AT_REMOVEDIR)==0,"Cannot discard duplicate staging");sync(impl->root.value);return id;}
    if(!authorization.empty())impl->save(output.value,authorizationFile,authorization);
    impl->point("generation-staging");
    uint64_t bytes=0;
    for(const auto& item:spec.files){
        impl->point("copy:"+item.first);auto from=impl->file(input.value,item.first);const auto value=info(from.value);need(value.st_size>=0&&uint64_t(value.st_size)<=maximumFile,"Invalid source generation file size");bytes+=uint64_t(value.st_size);need(bytes<=maximumPayload,"Generation exceeds its size limit");need(fs::space(impl->path).available>=uint64_t(value.st_size)+1024*1024,"Insufficient space to stage a reversible update");need(impl->digest(from.value)==item.second,"Source generation checksum mismatch");
        auto parent=descend(output.value,fs::path(item.first).parent_path(),true);Fd to(openat(parent.value,fs::path(item.first).filename().c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(to.value>=0,"Cannot copy generation file");
        std::array<char,65536> buffer{};uint64_t copied=0;
        for(;;){const auto n=read(from.value,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;need(n>=0,"Cannot read generation source");if(!n)break;copied+=uint64_t(n);need(copied<=uint64_t(value.st_size),"Generation source grew during copy");size_t at=0;while(at<size_t(n)){const auto written=write(to.value,buffer.data()+at,size_t(n)-at);if(written<0&&errno==EINTR)continue;need(written>0,"Cannot write generation copy");at+=size_t(written);}}
        need(copied==uint64_t(value.st_size),"Generation source changed during copy");sync(to.value);sync(parent.value);auto copiedFile=impl->file(output.value,item.first);need(impl->digest(copiedFile.value)==item.second,"Generation copy checksum mismatch");impl->point("copy-durable:"+item.first);
    }
    sync(output.value);impl->point("generation-ready");need(renameat(impl->root.value,temporary.c_str(),impl->root.value,id.c_str())==0,"Cannot publish complete generation");sync(impl->root.value);impl->point("generation-durable");impl->generation(id);return id;
}
void VersionStore::seed(const std::string& id){auto guard=impl->lock();need(!impl->state("active.json")&&!impl->state("healthy.json"),"Version store already initialized");impl->generation(id);Selection value{id,"",false,false};impl->record("healthy.json",value);impl->record("active.json",value);}
void VersionStore::activate(const std::string& id,const std::string& expected){
    auto guard=impl->lock();impl->generation(id);auto before=impl->state("active.json");need(before&&!before->pending,"Confirm or recover the previous update before activating another");impl->generation(before->active);
    need(expected.empty()||before->active==expected,"Selected version changed during update preparation");
    if(before->active==id)return;
    impl->record("healthy.json",{before->active,"",false,false});impl->record("active.json",{id,before->active,true,false});
}
std::optional<Generation> VersionStore::current(){auto guard=impl->lock();const auto value=impl->state("active.json");if(!value)return {};return impl->generation(value->active,value->pending);}
Generation VersionStore::inspect(const std::string& id){auto guard=impl->lock();return impl->generation(id);}
bool VersionStore::offer(const std::string& id){
    auto guard=impl->lock();need(!impl->profile.empty(),"A universal update offer requires a device profile");const auto candidate=impl->generation(id);need(candidate.spec.architecture=="universal"||candidate.spec.architecture==impl->profile,"Update offer does not support this device profile");const auto before=impl->state("active.json");need(before.has_value(),"A universal update offer needs an initialized profile");const auto selected=impl->generation(before->active);
    if(id==before->active||releaseVersion(candidate.spec.version)<=releaseVersion(selected.spec.version)||before->attempted)return false;
    const auto working=before->pending?impl->state("healthy.json"):before;need(working&&!working->pending,"Update offer lacks a verified fallback");impl->generation(working->active);impl->record("healthy.json",{working->active,"",false,false});impl->record("active.json",{id,working->active,true,false});return true;
}
std::optional<Generation> VersionStore::boot(){
    auto guard=impl->lock();std::optional<Selection> selected;
    try{selected=impl->state("active.json");}catch(const std::exception&){selected=impl->state("healthy.json");if(selected)impl->record("active.json",*selected);else throw;}
    if(!selected){selected=impl->state("healthy.json");if(!selected)return {};impl->record("active.json",*selected);}
    try{need(!selected->pending||!selected->attempted,"Unconfirmed generation did not start");auto result=impl->generation(selected->active,selected->pending);if(selected->pending){selected->attempted=true;impl->record("active.json",*selected);}return result;}
    catch(const std::exception&){
        const auto healthy=impl->state("healthy.json");const auto previous=selected->previous.empty()?(healthy?healthy->active:std::string()):selected->previous;
        need(!previous.empty()&&previous!=selected->active,"No verified fallback generation is available");auto result=impl->generation(previous);impl->record("healthy.json",{previous,"",false,false});impl->record("active.json",{previous,"",false,false});return result;
    }
}
// The open store pins the path; the generation ID prevents retained dlopen names from aliasing later versions.
fs::path VersionStore::entrypoint(const std::string& id,const std::string& role){auto guard=impl->lock();const auto generation=impl->generation(id);const auto found=generation.spec.entrypoints.find(role);need(found!=generation.spec.entrypoints.end(),"Generation has no requested entrypoint");return fs::path("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(impl->root.value))/id/found->second;}
void VersionStore::confirm(const std::string& id){auto guard=impl->lock();const auto current=impl->state("active.json");need(current&&current->active==id,"Stale generation health confirmation");impl->generation(id);if(!current->pending)return;need(current->attempted,"Generation has not been started");impl->record("healthy.json",{id,"",false,false});impl->record("active.json",{id,current->previous,false,false});}
void VersionStore::reject(const std::string& id){
    auto guard=impl->lock();auto current=impl->state("active.json");need(current&&current->active==id&&current->pending&&!current->previous.empty(),"No matching pending version can be rejected");impl->generation(current->previous);
    if(!current->attempted){current->attempted=true;impl->record("active.json",*current);}
    impl->record("healthy.json",{current->previous,"",false,false});impl->record("active.json",{current->previous,"",false,false});
}
void VersionStore::restore(const std::string& knownWorking,const std::string& expected){
    auto guard=impl->lock();const auto current=impl->state("active.json");need(current&&current->active==expected,"Selected version changed before recovery");impl->generation(knownWorking);if(current->active==knownWorking)return;
    impl->record("active.json",{expected,knownWorking,true,true});impl->record("healthy.json",{knownWorking,"",false,false});impl->record("active.json",{knownWorking,"",false,false});
}
void VersionStore::prune(){
    auto guard=impl->lock();impl->recoverStaging();const auto current=impl->state("active.json");need(current&&!current->pending,"Unconfirmed generations cannot be retired");impl->generation(current->active);if(!current->previous.empty())impl->generation(current->previous);
    std::set<std::string> retained;
    for(const auto& name:{"active.json","healthy.json","active.armv7.json","healthy.armv7.json","active.aarch64.json","healthy.aarch64.json"}){const auto state=impl->namedState(name);if(state){retained.insert(state->active);if(!state->previous.empty())retained.insert(state->previous);}}
    auto entries=listing(impl->root.value);std::vector<std::string> names;while(auto* entry=nextEntry(entries.get()))if(hex(entry->d_name)&&!retained.count(entry->d_name))names.emplace_back(entry->d_name);
    for(const auto& name:names){impl->recheck();const auto old=impl->generation(name);const auto retired=".retired-"+name;need(!present(impl->root.value,retired)&&renameat(impl->root.value,name.c_str(),impl->root.value,retired.c_str())==0,"Cannot retire old generation");sync(impl->root.value);impl->point("retirement-started");impl->removeGeneration(retired,old.spec);}
}
unsigned VersionStore::erase(){
    auto guard=impl->lock();unsigned preserved=0;
    try{impl->recoverStaging();}catch(const std::exception&){++preserved;}
    for(const auto& name:impl->names(impl->root.value)){
        if(!hex(name))continue;
        try{const auto old=impl->generation(name);const auto retired=".retired-"+name;need(!present(impl->root.value,retired)&&renameat(impl->root.value,name.c_str(),impl->root.value,retired.c_str())==0,"Cannot retire removed generation");sync(impl->root.value);impl->removeGeneration(retired,old.spec);}catch(const std::exception&){++preserved;}
    }
    for(const auto& name:{"active.json","healthy.json","active.armv7.json","healthy.armv7.json","active.aarch64.json","healthy.aarch64.json"})try{if(impl->namedState(name)){need(unlinkat(impl->root.value,name,0)==0,"Cannot remove version selection");sync(impl->root.value);}}catch(const std::exception&){++preserved;}
    return preserved;
}
}
