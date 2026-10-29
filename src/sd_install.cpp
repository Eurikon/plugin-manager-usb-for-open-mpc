#include "sd_install.hpp"
#include "startup/storage.h"
#include "display.hpp"
#include "source_runtime.hpp"
#include "crypto.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {
constexpr const char* capability="open-mpc-manager-sd-v1\n";
constexpr const char* configuration="open-mpc-manager-storage-v1\n";
constexpr const char* marker="mpc-usb-manager-v1\n";
constexpr const char* ownerFile=".manager-install.json";
const fs::path installedFolder="Plugin Manager USB";
const std::array<const char*,6> runtimeFiles={"manager","font.ttf","launch.sh","setup.sh","shortcut","shortcut-hook.so"};

void need(bool value,const std::string& message) { if(!value)throw std::runtime_error(message); }
bool present(const fs::path& path) { struct stat st{};return lstat(path.c_str(),&st)==0; }
void plainPath(const fs::path& path) {
    need(path.is_absolute()&&path.lexically_normal()==path,"Invalid storage path");
    fs::path current;
    for(const auto& part:path){current/=part;struct stat st{};need(lstat(current.c_str(),&st)==0&&!S_ISLNK(st.st_mode),"Storage path is missing or contains a link");}
}
struct OperationLock {
    int fd=-1;
    OperationLock(const fs::path& session,uid_t owner) {
        plainPath(session);fd=open((session/"sd-install.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        struct stat st{};
        if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=owner||(st.st_mode&0022)||st.st_nlink!=1||flock(fd,LOCK_EX|LOCK_NB)){
            if(fd>=0)close(fd);
            throw std::runtime_error("Another SD operation is running or its lock is unsafe");
        }
    }
    ~OperationLock(){if(fd>=0)close(fd);}
};
bool helperExfat(const fs::path& path);
std::string readFile(const fs::path& path,size_t limit,bool owned=false,uid_t owner=0) {
    plainPath(path.parent_path());
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    need(fd>=0,"Cannot read "+path.filename().string());
    struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||static_cast<uint64_t>(st.st_size)>limit
       ||(owned&&(st.st_uid!=owner||((st.st_mode&0022)&&!helperExfat(path))||st.st_nlink!=1))){close(fd);throw std::runtime_error("Unsafe or oversized "+path.filename().string());}
    std::string text(static_cast<size_t>(st.st_size),'\0');size_t position=0;
    while(position<text.size()){const auto count=::read(fd,text.data()+position,text.size()-position);if(count<0&&errno==EINTR)continue;if(count<=0){close(fd);throw std::runtime_error("Storage file changed while reading");}position+=static_cast<size_t>(count);}
    char extra;const auto end=::read(fd,&extra,1);close(fd);need(end==0,"Storage file changed while reading");return text;
}
void syncDirectory(const fs::path& path) {
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);need(fd>=0,"Cannot open storage directory");
    const int result=fsync(fd);close(fd);need(result==0,"Cannot sync storage directory");
}
void saveExclusive(const fs::path& path,const std::string& text,mode_t mode=0644) {
    plainPath(path.parent_path());int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode);
    need(fd>=0,"Cannot create "+path.filename().string());size_t position=0;bool valid=true;
    while(position<text.size()){const auto count=::write(fd,text.data()+position,text.size()-position);if(count<0&&errno==EINTR)continue;if(count<=0){valid=false;break;}position+=static_cast<size_t>(count);}
    if(fsync(fd))valid=false;
    if(close(fd))valid=false;
    need(valid,"Cannot save storage file");
}
void makeDirectory(const fs::path& path) {
    if(present(path)){plainPath(path);need(fs::is_directory(path),"Storage folder is occupied by a file");return;}
    plainPath(path.parent_path());need(mkdir(path.c_str(),0755)==0,"Cannot create storage folder");
}
void makeParents(const fs::path& root,const fs::path& relative) {
    fs::path path=root;for(const auto& part:relative){need(part!=".."&&part!=".","Invalid relative folder");path/=part;makeDirectory(path);}
}
bool uuidValid(const std::string& uuid) { return !uuid.empty()&&uuid.size()<=64&&uuid.find_first_not_of("abcdefABCDEF0123456789-")==std::string::npos; }
bool digestValid(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
bool helperReady(const SdInstallPaths& paths) {
    plainPath(paths.helper);struct stat st{};
    need(lstat(paths.helper.c_str(),&st)==0&&S_ISDIR(st.st_mode)&&st.st_uid==paths.owner&&(!(st.st_mode&0022)||helperExfat(paths.helper)),"Unsafe startup helper");
    return readFile(paths.helper/"sd-install-capability",64,true,paths.owner)==capability;
}
void validatePlatform(const SdInstallPaths& paths) {
    need(helperReady(paths),"SD installation requires the updated MPC 3.10 helper");
    need(geteuid()==paths.owner,"SD installation requires the manager service");plainPath(paths.firmware);
    need(sha256(paths.firmware)==paths.firmwareHash,"SD installation is supported only on the tested MPC OS 3.10 firmware");
}
std::string configuredUuid(const SdInstallPaths& paths,const char* name="installed-storage") {
    const auto path=paths.helper/name;if(!present(path))return {};
    struct stat st{};need(lstat(path.c_str(),&st)==0&&((st.st_mode&07777)==0600||helperExfat(path)),"Unsafe saved SD selection");
    const auto content=readFile(path,160,true,paths.owner);const auto prefix=std::string(configuration);
    need(content.compare(0,prefix.size(),prefix)==0&&content.size()>prefix.size()&&content.back()=='\n',"Invalid saved SD selection");
    const auto uuid=content.substr(prefix.size(),content.size()-prefix.size()-1);need(uuidValid(uuid),"Invalid saved SD identity");return uuid;
}
void saveConfiguration(const SdInstallPaths& paths,const std::string& uuid,const char* name="installed-storage") {
    const auto target=paths.helper/name, temporary=paths.helper/(std::string(".")+name+".tmp");
    need(!present(temporary),"An unfinished SD activation must be checked before retrying");
    saveExclusive(temporary,std::string(configuration)+uuid+'\n',0600);
    try{need(chmod(temporary.c_str(),0600)==0,"Cannot protect saved SD selection");fs::rename(temporary,target);syncDirectory(paths.helper);}
    catch(...){std::error_code error;fs::remove(temporary,error);throw;}
}
std::string decode(const std::string& input) {
    std::string out;
    for(size_t i=0;i<input.size();++i){char c=input[i];if(c=='\\'){need(i+3<input.size(),"Invalid mount escape");const auto code=input.substr(i+1,3);need(code=="040"||code=="134","Unsupported mount name");c=code=="040"?' ':'\\';i+=3;}need(static_cast<unsigned char>(c)>=32&&c!=127,"Unsupported mount name");out+=c;}
    return out;
}
struct Volume {std::string uuid,label;fs::path root;dev_t device;unsigned long mountId;uint64_t freeBytes;};
bool writableMount(const std::string& value){return value=="rw"||value.compare(0,3,"rw,")==0||value.find(",rw,")!=std::string::npos||(value.size()>3&&value.compare(value.size()-3,3,",rw")==0);}
std::string deviceNumber(dev_t device){return std::to_string(major(device))+':'+std::to_string(minor(device));}
std::string deviceMetadata(const fs::path& path,size_t limit) {
    plainPath(path.parent_path());const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    need(fd>=0,"Storage media metadata is unavailable");struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);throw std::runtime_error("Invalid storage media metadata");}
    std::string value;std::array<char,4096> buffer;
    for(;;){const auto count=::read(fd,buffer.data(),buffer.size());if(count<0&&errno==EINTR)continue;
        if(count<0||value.size()+static_cast<size_t>(count)>limit){close(fd);throw std::runtime_error("Invalid storage media metadata");}
        if(!count)break;
        value.append(buffer.data(),static_cast<size_t>(count));}
    close(fd);return value;
}
bool sdDevice(const SdInstallPaths& paths,dev_t device) noexcept {
    try {
        const auto devices=fs::canonical(paths.sys/"devices"),block=fs::canonical(paths.sys/"dev/block"/deviceNumber(device));
        const auto relative=block.lexically_relative(devices);
        if(relative.empty()||*relative.begin()==".."||deviceMetadata(block/"dev",64)!=deviceNumber(device)+'\n')return false;
        auto disk=block;
        if(present(block/"partition")){
            const auto partition=deviceMetadata(block/"partition",32);
            if(partition.size()<2||partition.back()!='\n'||partition[0]<'1'||partition[0]>'9'
               ||partition.substr(0,partition.size()-1).find_first_not_of("0123456789")!=std::string::npos)return false;
            disk=block.parent_path();
        }
        const auto parent=deviceMetadata(disk/"dev",64);
        unsigned majorValue=0,minorValue=0;char end=0;
        if(sscanf(parent.c_str(),"%u:%u%c",&majorValue,&minorValue,&end)!=3||end!='\n'
           ||parent!=std::to_string(majorValue)+':'+std::to_string(minorValue)+'\n'
           ||fs::canonical(paths.sys/"dev/block"/parent.substr(0,parent.size()-1))!=disk
           ||deviceMetadata(disk/"removable",32)!="1\n")return false;
        std::istringstream properties(deviceMetadata(paths.udev/("b"+parent.substr(0,parent.size()-1)),65536));
        std::string line;bool sd=false;
        while(std::getline(properties,line))if(line.compare(0,20,"E:ID_DRIVE_FLASH_SD=")==0){
            if(sd||line!="E:ID_DRIVE_FLASH_SD=1")return false;
            sd=true;
        }
        return sd;
    }catch(...){return false;}
}
std::vector<Volume> volumes(const SdInstallPaths& paths,const fs::path& source,bool includeSource,bool requireSd=true) {
    plainPath(paths.media);plainPath(paths.uuids);
    std::ifstream input(paths.mountinfo);need(bool(input),"Cannot read mounted cards");
    std::vector<Volume> found;std::map<dev_t,unsigned> counts;std::string line;
    std::set<dev_t> internal={paths.fileDevice(paths.internal)};const dev_t sourceDevice=paths.fileDevice(source);
    for(const auto& path:paths.protectedRoots)if(present(path))internal.insert(paths.fileDevice(path));
    while(std::getline(input,line)){
        need(line.size()<16384,"Invalid mount inventory");std::istringstream fields(line);unsigned long id,parent;std::string device,root,point,options,token,type;
        need(bool(fields>>id>>parent>>device>>root>>point>>options),"Invalid mount inventory");
        while(fields>>token&&token!="-"){}need(token=="-"&&bool(fields>>type),"Invalid mount filesystem");
        fs::path mount=decode(point);if(mount.parent_path()!=paths.media||root!="/"||!writableMount(options)||(type!="exfat"&&type!="vfat"&&type!="ext4"))continue;
        plainPath(mount);const auto number=paths.fileDevice(mount);
        need(device==std::to_string(major(number))+':'+std::to_string(minor(number)),"Mounted card identity changed");
        if(internal.count(number)||(!includeSource&&number==sourceDevice)||(requireSd&&!sdDevice(paths,number)))continue;
        ++counts[number];std::string uuid;
        for(const auto& entry:fs::directory_iterator(paths.uuids)){
            const auto name=entry.path().filename().string();if(!uuidValid(name))continue;
            try{if(paths.blockDevice(entry.path())!=number)continue;}catch(const std::exception&){continue;}
            need(uuid.empty(),"Card has ambiguous filesystem identities");uuid=name;
        }
        if(uuid.empty())continue;
        found.push_back({uuid,mount.filename().string(),mount,number,id,paths.availableBytes(mount)});
    }
    for(const auto& item:found)need(counts.at(item.device)==1,"Card is mounted more than once");
    std::set<std::string> identities;for(const auto& item:found)need(identities.insert(item.uuid).second,"Duplicate card identity");
    return found;
}
Volume selectVolume(const SdInstallPaths& paths,const fs::path& source,const std::string& uuid,bool includeSource,bool requireSd=true) {
    need(uuidValid(uuid),"Invalid selected SD identity");
    for(const auto& item:volumes(paths,source,includeSource,requireSd))if(item.uuid==uuid)return item;
    throw std::runtime_error("Selected storage is unavailable, read-only or not an identified removable SD card");
}
void unchanged(const SdInstallPaths& paths,const fs::path& source,const Volume& volume,bool requireSd=true) {
    const auto current=selectVolume(paths,source,volume.uuid,true,requireSd);
    need(current.root==volume.root&&current.device==volume.device&&current.mountId==volume.mountId,"SD card changed during installation");
}
bool helperExfat(const fs::path& path) {
    auto parent=path;
    while(parent!=parent.root_path()&&!pmu_helper_directory_valid(parent.c_str()))parent=parent.parent_path();
    if(!pmu_helper_directory_valid(parent.c_str()))return false;
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return false;
    const bool valid=pmu_helper_exfat(fd);close(fd);return valid;
}
bool ownedName(const std::string& name) {
    if(name==".mpc-manager-id")return true;
    for(const auto* file:runtimeFiles)if(name==std::string("autorun/runtime/")+file||name==std::string("autorun/runtime/")+managerRuntimePath(file,"aarch64"))return true;
    const std::string prefix="autorun/runtime/licenses/";if(name.compare(0,prefix.size(),prefix)!=0||name.size()<=prefix.size())return false;
    const fs::path relative(name);if(relative.generic_string()!=name)return false;
    for(const auto& part:relative)if(part==".."||part=="."||part.empty())return false;
    return name.find_first_of("\r\n\\")==std::string::npos;
}
Json receipt(const fs::path& target,const std::string& uuid) {
    plainPath(target);const auto value=Json::parse(readFile(target/ownerFile,128*1024));
    need(value.is_object()&&(value.value("schema",0)==1||(value.value("schema",0)==2&&value.value("layout","")=="generations-v1"))&&value.value("owner","")=="plugin-manager-usb"
        &&value.value("uuid","")==uuid&&value.contains("installed")&&value["installed"].is_boolean()
        &&value.contains("files")&&value["files"].is_object()&&value["files"].size()<=256,"Unrecognized SD installation folder");
    for(const auto& item:value["files"].items())need(ownedName(item.key())&&item.value().is_string()&&digestValid(item.value().get<std::string>()),"Invalid SD ownership manifest");
    return value;
}
void safeRuntimeTree(const fs::path& target,const Json& owner,bool versioned=false) {
    const auto runtime=target/"autorun/runtime";if(!present(runtime))return;plainPath(runtime);
    for(auto iterator=fs::recursive_directory_iterator(runtime);iterator!=fs::recursive_directory_iterator();++iterator){
        const auto& entry=*iterator;
        const auto relative=entry.path().lexically_relative(target).generic_string();
        need(!fs::is_symlink(entry.symlink_status()),"SD runtime contains a link");
        if(versioned&&entry.path()==runtime/".pmu-versions"){need(entry.is_directory(),"SD version storage is occupied by a file");iterator.disable_recursion_pending();continue;}
        if(entry.is_directory())continue;
        need(entry.is_regular_file()&&owner["files"].contains(relative),"SD runtime contains unowned files; preserved without updating");
        need(sha256(entry.path())==owner["files"][relative].get<std::string>(),"SD runtime was modified; preserved without updating");
    }
}
std::map<std::string,std::string> runtimeManifest(const SdInstallPaths& paths) {
    std::istringstream input(readFile(paths.helper/managerRuntimeManifest(paths.arch),1024,true,paths.owner));std::map<std::string,std::string> result;std::string line;
    for(const auto* name:runtimeFiles){need(bool(std::getline(input,line))&&line.size()==66+managerRuntimePath(name,paths.arch).size()&&line.substr(64)==std::string("  ")+managerRuntimePath(name,paths.arch)&&digestValid(line.substr(0,64)),"Invalid startup runtime manifest");result[name]=line.substr(0,64);}
    need(!std::getline(input,line),"Unexpected startup runtime manifest entry");return result;
}
void copyVerified(const fs::path& source,const fs::path& target,const std::string& expected) {
    plainPath(source);need(fs::is_regular_file(source)&&fs::file_size(source)<=64*1024*1024,"Invalid runtime source file");plainPath(target.parent_path());
    int input=open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);need(input>=0,"Cannot read installation source");
    int output=open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644);if(output<0){close(input);throw std::runtime_error("Cannot write SD runtime");}
    bool valid=true;std::array<char,65536> block;ssize_t count;uint64_t size=0;
    while((count=::read(input,block.data(),block.size()))!=0){if(count<0){if(errno==EINTR)continue;valid=false;break;}size+=static_cast<uint64_t>(count);if(size>64*1024*1024){valid=false;break;}ssize_t offset=0;while(offset<count){const auto written=::write(output,block.data()+offset,static_cast<size_t>(count-offset));if(written<0&&errno==EINTR)continue;if(written<=0){valid=false;break;}offset+=written;}if(!valid)break;}
    close(input);if(fsync(output))valid=false;if(close(output))valid=false;need(valid,"SD write failed; installation not activated");
    need(sha256(target)==expected,"SD copy verification failed");
}
fs::path stageDirectory(const fs::path& parent) {
    std::string value=(parent/".plugin-manager-stage.XXXXXX").string();std::vector<char> name(value.begin(),value.end());name.push_back(0);
    char* result=mkdtemp(name.data());need(result!=nullptr,"Cannot stage SD installation");return result;
}
void removeEmptyParents(const fs::path& target,const fs::path& relative) {
    auto path=(target/relative).parent_path();while(path!=target&&path.string().size()>target.string().size()) {std::error_code error;const bool removed=fs::remove(path,error);if(error||!removed)break;path=path.parent_path();}
}
pmu::VersionStore versionStore(const fs::path& target,const SdInstallPaths& paths,const std::string& profile,bool create=true){
    return {target/"autorun/runtime/.pmu-versions",paths.owner,[](const auto& file){return sha256(file);},{},create,[key=paths.publisher](const auto& id,const auto&,const auto& proof){pmu::verifyAuthorization(key,"runtime",id,proof);},profile};
}
unsigned clearFlatRuntime(const SdInstallPaths& paths,const fs::path& target,const Volume& volume,const fs::path& source,const Json& files){
    unsigned preserved=0;for(const auto& item:files.items()){
        if(item.key()==".mpc-manager-id")continue;
        unchanged(paths,source,volume,false);const auto file=target/item.key();if(!present(file))continue;
        try{plainPath(file);if(fs::is_regular_file(file)&&sha256(file)==item.value().get<std::string>()){fs::remove(file);removeEmptyParents(target,item.key());}else ++preserved;}catch(const std::exception&){++preserved;}
    }return preserved;
}
void installVersioned(const SdInstallPaths& paths,const fs::path& source,const Volume& volume,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount){
    pmu::SourceRuntime runtime(source,paths.arch,paths.publisher);const auto selected=runtime.choose(false);need(!selected.generation.pending,"Wait for manager startup confirmation before installing to SD");uint64_t bytes=0;for(const auto& file:selected.generation.spec.files)bytes+=fs::file_size(selected.generation.directory/file.first);need(bytes<=128ULL*1024*1024&&volume.freeBytes>=bytes+1024*1024,"Insufficient space on the selected SD card");
    const auto target=volume.root/installedFolder;const bool existing=present(target);bool previouslyInstalled=false;Json oldFiles=Json::object();if(existing){const auto owner=receipt(target,volume.uuid);previouslyInstalled=owner.at("installed").get<bool>();if(owner.value("schema",0)==1){if(previouslyInstalled)safeRuntimeTree(target,owner,true);oldFiles=owner.at("files");previouslyInstalled=false;}if(present(target/".mpc-manager-id"))need(readFile(target/".mpc-manager-id",64)==marker,"Modified SD marker was preserved");}
    const auto stage=stageDirectory(volume.root);const auto destination=existing?target:stage;bool published=false;
    try{
        if(progress)progress("Verifying and copying the complete manager to SD...");
        if(amount)amount(0);
        makeParents(destination,"autorun/runtime");
        auto selectedStore=versionStore(destination,paths,paths.arch);const auto id=selectedStore.stage(selected.generation.directory,selected.generation.spec,selected.generation.authorization);runtime.choose(false,selected.identity);unchanged(paths,source,volume);
        if(existing){
            if(selectedStore.current())selectedStore.offer(id);else{need(!previouslyInstalled,"The installed SD manager needs recovery before updating");selectedStore.seed(id);}
            for(const auto& architecture:{std::string("armv7"),std::string("aarch64")})if(architecture!=paths.arch&&selected.generation.spec.entrypoints.count("manager_"+architecture)&&selected.generation.spec.entrypoints.count("runtime_worker_"+architecture)){auto other=versionStore(destination,paths,architecture);if(other.current())other.offer(id);else{need(!previouslyInstalled,"The installed SD manager profile needs recovery");other.seed(id);}}
            if(!present(target/".mpc-manager-id"))saveExclusive(target/".mpc-manager-id",marker);
            auto owner=receipt(target,volume.uuid);owner["schema"]=2;owner["layout"]="generations-v1";owner["installed"]=true;if(oldFiles.empty())oldFiles=owner.at("files");oldFiles[".mpc-manager-id"]=sha256(target/".mpc-manager-id");owner["files"]=oldFiles;saveExclusive(stage/ownerFile,owner.dump(2)+'\n');fs::rename(stage/ownerFile,target/ownerFile);syncDirectory(target);
        }
        else{
            selectedStore.seed(id);for(const auto& architecture:{std::string("armv7"),std::string("aarch64")})if(architecture!=paths.arch&&selected.generation.spec.entrypoints.count("manager_"+architecture)&&selected.generation.spec.entrypoints.count("runtime_worker_"+architecture)){auto other=versionStore(destination,paths,architecture);other.seed(id);}
            saveExclusive(destination/".mpc-manager-id",marker);saveExclusive(destination/ownerFile,Json({{"schema",2},{"layout","generations-v1"},{"owner","plugin-manager-usb"},{"uuid",volume.uuid},{"installed",true},{"files",{{".mpc-manager-id",sha256(destination/".mpc-manager-id")}}}}).dump()+'\n');makeDirectory(destination/"local-plugins");
            for(const auto* name:{"manager-preferences.json","plugin-history.json"})if(present(source/name)){const auto text=readFile(source/name,8*1024*1024);need(Json::parse(text).is_object(),"Invalid saved manager data");saveExclusive(destination/name,text);}
            if(present(source/"display-rotation.txt")){std::string note;const bool flipped=displayFlipFromFile((source/"display-rotation.txt").string(),note);if(note.empty())saveExclusive(destination/"display-rotation.txt",flipped?"180\n":"0\n");}
            syncDirectory(destination);unchanged(paths,source,volume);fs::rename(stage,target);published=true;syncDirectory(target.parent_path());
        }
        unchanged(paths,source,volume);saveConfiguration(paths,volume.uuid);const auto preserved=clearFlatRuntime(paths,target,volume,source,oldFiles);if(existing)fs::remove_all(stage);if(amount)amount(1);if(progress)progress(preserved?"Manager installed on SD. Changed old files were preserved. Return to MPC to finish setup.":"Manager installed on SD. Return to MPC to finish setup.");
    }catch(...){if(existing||!published){std::error_code ignored;fs::remove_all(stage,ignored);}throw;}
}
void removeVersioned(const SdInstallPaths& paths,const fs::path& target,const Volume& volume,const fs::path& source,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount){
    unchanged(paths,source,volume,false);if(progress)progress("Removing the verified manager versions from SD...");saveConfiguration(paths,volume.uuid,"installed-storage.removing");fs::remove(paths.helper/"installed-storage");syncDirectory(paths.helper);const auto markerPath=target/".mpc-manager-id";if(present(markerPath)){need(readFile(markerPath,64)==marker,"Modified SD marker was preserved");fs::remove(markerPath);syncDirectory(target);}
    unsigned preserved=clearFlatRuntime(paths,target,volume,source,receipt(target,volume.uuid).at("files"));const auto pool=target/"autorun/runtime/.pmu-versions";if(present(pool)){{auto versions=versionStore(target,paths,paths.arch,false);preserved+=versions.erase();}const auto lock=pool/"update.lock";if(present(lock)){const auto text=readFile(lock,0,true,paths.owner);need(text.empty(),"Unexpected generation lock content");fs::remove(lock);}std::error_code ignored;fs::remove(pool,ignored);fs::remove(target/"autorun/runtime",ignored);fs::remove(target/"autorun",ignored);}
    auto owner=receipt(target,volume.uuid);owner["installed"]=false;owner["files"]=Json::object();const auto temporary=target/".manager-install.tmp";saveExclusive(temporary,owner.dump(2)+'\n');fs::rename(temporary,target/ownerFile);syncDirectory(target);unchanged(paths,source,volume,false);fs::remove(paths.helper/"installed-storage.removing");syncDirectory(paths.helper);if(amount)amount(1);if(progress)progress(preserved?"SD manager removed. Modified files and saved data were preserved.":"SD manager removed. Saved settings, history and local packages were preserved.");
}
SdInstallPaths defaults() {
    SdInstallPaths paths;
#if defined(__aarch64__)
    if(const char* helper=std::getenv("MPC_MANAGER_SD_HELPER");helper&&pmu_helper_directory_valid(helper)) {
        paths.helper=helper;
        paths.internal=paths.helper.parent_path().parent_path().parent_path();
    }
#endif
    paths.fileDevice=[](const fs::path& path){struct stat st{};need(stat(path.c_str(),&st)==0,"Storage device is unavailable");return st.st_dev;};
    paths.blockDevice=[](const fs::path& path){struct stat st{};need(stat(path.c_str(),&st)==0&&S_ISBLK(st.st_mode),"Filesystem identity is not a block device");return st.st_rdev;};
    paths.availableBytes=[](const fs::path& path){return fs::space(path).available;};
    return paths;
}
}

SdInstaller::SdInstaller(fs::path root,fs::path cache):SdInstaller(std::move(root),std::move(cache),defaults()) {
    const char* helper=std::getenv("MPC_MANAGER_SD_HELPER");enabled=helper&&paths.helper==helper;
}
SdInstaller::SdInstaller(fs::path root,fs::path cache,SdInstallPaths locations):source(std::move(root)),session(std::move(cache)),paths(std::move(locations)) {
    const auto fallback=defaults();if(!paths.fileDevice)paths.fileDevice=fallback.fileDevice;if(!paths.blockDevice)paths.blockDevice=fallback.blockDevice;
    if(!paths.availableBytes)paths.availableBytes=fallback.availableBytes;
}
bool SdInstaller::available() const noexcept {try{return enabled&&helperReady(paths);}catch(...){return false;}}
std::vector<Json> SdInstaller::targets() {
    need(available(),"SD installation is unavailable on this firmware");const auto configured=configuredUuid(paths);std::vector<Json> result;
    for(const auto& volume:volumes(paths,source,true)){
        if(volume.device==paths.fileDevice(source)&&source!=volume.root/installedFolder)continue;
        result.push_back({{"uuid",volume.uuid},{"label",volume.label},{"path",volume.root.string()},{"free_bytes",volume.freeBytes},{"selected",volume.uuid==configured}});
    }
    return result;
}
Json SdInstaller::inspect() {
    Json value={{"supported",available()},{"configured",false},{"installed",false},{"current",false},{"uuid",""},{"label",""},{"path",""},{"can_remove",false},{"reason",""}};
    if(!value["supported"].get<bool>())return value;
    const auto configured=configuredUuid(paths),pending=configuredUuid(paths,"installed-storage.removing");
    need(configured.empty()||pending.empty()||configured==pending,"Conflicting saved SD selections");
    const auto uuid=pending.empty()?configured:pending;if(uuid.empty())return value;
    value["configured"]=!configured.empty();value["removing"]=!pending.empty();value["uuid"]=uuid;
    for(const auto& volume:volumes(paths,source,true,false))if(volume.uuid==uuid){
        const auto target=volume.root/installedFolder;const auto owned=receipt(target,uuid);
        value["label"]=volume.label;value["path"]=target.string();value["current"]=source==target;value["can_remove"]=true;
        if(owned.value("schema",0)==2){bool complete=false;try{pmu::SourceRuntime runtime(target,paths.arch,paths.publisher);complete=runtime.choose(false).generation.spec.entrypoints.count("manager_"+paths.arch);}catch(const std::exception&){}value["installed"]=owned["installed"].get<bool>()&&complete;}
        else value["installed"]=owned["installed"].get<bool>()&&present(target/".mpc-manager-id")&&present(target/"autorun/runtime"/managerRuntimePath("manager",paths.arch));
        if(!pending.empty()){value["installed"]=false;value["reason"]="SD removal was interrupted. Select Remove to finish.";}
        else if(!value["installed"].get<bool>())value["reason"]="SD installation is incomplete. Reinstall or remove it.";
        return value;
    }
    value["reason"]="The installed SD card is absent or read-only.";return value;
}
void SdInstaller::install(const std::string& uuid,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount) {
    OperationLock lock(session,paths.owner);
    need(available(),"SD installation is unavailable on this firmware");validatePlatform(paths);const auto prior=configuredUuid(paths);
    need(configuredUuid(paths,"installed-storage.removing").empty(),"Finish the interrupted SD removal before installing");
    need(prior.empty()||prior==uuid,"Remove the existing SD installation before selecting another card");
    const auto volume=selectVolume(paths,source,uuid,true);const auto target=volume.root/installedFolder;
    need(volume.device!=paths.fileDevice(source)||source==target,"Choose the SD card, not the installation USB");
    if(present(source/"autorun/runtime/.pmu-versions")){installVersioned(paths,source,volume,progress,amount);return;}
    const auto expected=runtimeManifest(paths);const auto runtime=source/"autorun/runtime";plainPath(runtime);
    need(readFile(source/".mpc-manager-id",64)==marker,"Invalid manager source");
    std::map<std::string,std::string> files;uint64_t bytes=0;
    for(const auto* name:runtimeFiles){const auto relative=managerRuntimePath(name,paths.arch);plainPath(runtime/relative);need(fs::is_regular_file(runtime/relative)&&fs::file_size(runtime/relative)<=64*1024*1024,"Runtime source is not a supported file");need(sha256(runtime/relative)==expected.at(name),"Installation source differs from the verified manager runtime");files[std::string("autorun/runtime/")+relative]=expected.at(name);bytes+=fs::file_size(runtime/relative);}
    const auto licenses=runtime/"licenses";
    if(present(licenses)){plainPath(licenses);for(const auto& entry:fs::recursive_directory_iterator(licenses)){
        need(!entry.is_symlink(),"License source contains a link");if(entry.is_directory())continue;
        const auto relative=entry.path().lexically_relative(source).generic_string();need(entry.is_regular_file()&&ownedName(relative)&&files.size()<256&&fs::file_size(entry.path())<=1024*1024,"Invalid license source");
        files[relative]=sha256(entry.path());bytes+=fs::file_size(entry.path());}}
    need(bytes<=128*1024*1024&&volume.freeBytes>=bytes+1024*1024,"Insufficient space on the selected SD card");
    const bool updating=present(target);Json previous;bool hadMarker=false;
    if(updating){previous=receipt(target,uuid);safeRuntimeTree(target,previous);hadMarker=present(target/".mpc-manager-id");if(hadMarker)need(readFile(target/".mpc-manager-id",64)==marker,"SD marker was modified; preserved without updating");}
    const auto stage=stageDirectory(volume.root);bool movedOld=false,movedNew=false,newTarget=false,activated=false;
    const auto oldRuntime=stage/"previous-runtime";
    try {
        if(progress)progress("Copying manager to SD...");
        if(amount)amount(0);
        makeParents(stage,"autorun/runtime");size_t done=0;
        for(const auto& item:files){unchanged(paths,source,volume);makeParents(stage,fs::path(item.first).parent_path());copyVerified(source/item.first,stage/item.first,item.second);if(amount)amount(float(++done)/float(files.size()+2));}
        saveExclusive(stage/".mpc-manager-id",marker);files[".mpc-manager-id"]=sha256(stage/".mpc-manager-id");
        Json owner={{"schema",1},{"owner","plugin-manager-usb"},{"uuid",uuid},{"installed",true},{"files",files}};
        saveExclusive(stage/ownerFile,owner.dump(2)+'\n');makeDirectory(stage/"local-plugins");
        if(!updating)for(const char* name:{"manager-preferences.json","plugin-history.json"})if(present(source/name)){
            const auto text=readFile(source/name,8*1024*1024);const auto parsed=Json::parse(text);need(parsed.is_object(),"Invalid saved manager data");saveExclusive(stage/name,text);
        }
        if(!updating && present(source/"display-rotation.txt")) {
            std::string note;
            const bool flipped=displayFlipFromFile((source/"display-rotation.txt").string(),note);
            if(note.empty())saveExclusive(stage/"display-rotation.txt",flipped?"180\n":"0\n");
        }
        syncDirectory(stage/"autorun/runtime");syncDirectory(stage);unchanged(paths,source,volume);
        if(progress)progress("Activating SD installation...");
        if(updating){
            makeDirectory(target/"autorun");if(present(target/"autorun/runtime")){fs::rename(target/"autorun/runtime",oldRuntime);movedOld=true;}
            fs::rename(stage/"autorun/runtime",target/"autorun/runtime");movedNew=true;
            fs::rename(target/ownerFile,stage/"previous-owner.json");fs::rename(stage/ownerFile,target/ownerFile);
            if(!present(target/".mpc-manager-id"))fs::rename(stage/".mpc-manager-id",target/".mpc-manager-id");
            syncDirectory(target/"autorun");syncDirectory(target);
        }else{fs::rename(stage,target);newTarget=true;syncDirectory(target.parent_path());}
        unchanged(paths,source,volume);saveConfiguration(paths,uuid);activated=true;
        if(updating)fs::remove_all(stage);
        if(amount)amount(1);
        if(progress)progress("Manager installed on SD. Return to MPC to finish setup.");
    }catch(...){
        if(!activated){try{unchanged(paths,source,volume);if(prior.empty()&&configuredUuid(paths)==uuid){fs::remove(paths.helper/"installed-storage");syncDirectory(paths.helper);}if(newTarget){fs::remove_all(target);}else if(updating){
            if(movedNew)fs::remove_all(target/"autorun/runtime");
            if(movedOld)fs::rename(oldRuntime,target/"autorun/runtime");
            if(present(stage/"previous-owner.json")){fs::remove(target/ownerFile);fs::rename(stage/"previous-owner.json",target/ownerFile);}
            if(!hadMarker)fs::remove(target/".mpc-manager-id");
        }if(present(stage))fs::remove_all(stage);}catch(...) {}}
        throw;
    }
}
void SdInstaller::remove(const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount) {
    OperationLock lock(session,paths.owner);
    need(available(),"SD installation is unavailable on this firmware");validatePlatform(paths);
    const auto active=configuredUuid(paths),pending=configuredUuid(paths,"installed-storage.removing");
    need(active.empty()||pending.empty()||active==pending,"Conflicting saved SD selections");const auto uuid=pending.empty()?active:pending;
    if(uuid.empty()){if(amount)amount(1);if(progress)progress("SD manager is already removed.");return;}
    const auto volume=selectVolume(paths,source,uuid,true,false);const auto target=volume.root/installedFolder;auto owned=receipt(target,uuid);
    if(owned.value("schema",0)==2){removeVersioned(paths,target,volume,source,progress,amount);return;}
    if(progress)progress("Removing manager from SD...");
    unchanged(paths,source,volume,false);
    if(pending.empty())saveConfiguration(paths,uuid,"installed-storage.removing");
    fs::remove(paths.helper/"installed-storage");syncDirectory(paths.helper);size_t done=0;unsigned preserved=0;
    for(const auto& item:owned["files"].items()){
        if(item.key()==".mpc-manager-id")continue;
        unchanged(paths,source,volume,false);const auto file=target/item.key();
        if(present(file)){
            try{plainPath(file);if(fs::is_regular_file(file)&&sha256(file)==item.value().get<std::string>()){fs::remove(file);removeEmptyParents(target,item.key());}else ++preserved;}
            catch(const std::exception&){++preserved;}
        }
        if(amount)amount(float(++done)/float(owned["files"].size()+1));
    }
    owned["installed"]=false;owned["files"]=Json::object();const auto temporary=target/".manager-install.tmp";
    saveExclusive(temporary,owned.dump(2)+'\n');fs::rename(temporary,target/ownerFile);syncDirectory(target);
    if(amount)amount(1);
    if(progress)progress(preserved?"SD manager removed. Modified files and saved data were preserved.":"SD manager removed. Saved settings, history and local packages were preserved.");
    unchanged(paths,source,volume,false);const auto identity=target/".mpc-manager-id";
    fs::remove(paths.helper/"installed-storage.removing");syncDirectory(paths.helper);
    if(present(identity)){need(readFile(identity,64)==marker,"Modified SD marker was preserved");fs::remove(identity);syncDirectory(target);}
}
