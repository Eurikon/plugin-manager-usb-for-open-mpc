#include "manager_update.hpp"
#include "startup/storage.h"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
namespace {
    const std::array<const char*,6> runtime={
        "manager","font.ttf","launch.sh","setup.sh","shortcut","shortcut-hook.so"
    };
    const std::set<std::string> helperNames={
        ".owner","PluginManager.xpj","bootstrap.sh","bootstrap-note","bootstrap-note-aarch64","controller.ini","manager-enabled","manager-entry.so","plugin_manager.lua","runtime.sha256","sd-install-capability","manager-entry-aarch64.so","runtime-aarch64.sha256"
    };
    constexpr const char* marker="mpc-usb-manager-v1\n";
    void need(bool value,const std::string& message){
        if(!value)throw std::runtime_error(message);
    }
    bool present(const fs::path& path){
        struct stat st{
        };
        return lstat(path.c_str(),&st)==0;
    }
    bool beneath(const fs::path& root,const fs::path& path){
        const auto rel=path.lexically_relative(root);
        return !rel.empty()&&*rel.begin()!=".."&&rel!=".";
    }
    void plain(const fs::path& path){
        need(path.is_absolute()&&path.lexically_normal()==path,"Unsafe manager update path");
        fs::path current;
        for(const auto& part:path){
            current/=part;
            struct stat st{
            };
            need(lstat(current.c_str(),&st)==0&&!S_ISLNK(st.st_mode),"Manager update path disappeared or contains a link");
        }
    }
    struct Identity {
        dev_t device;
        ino_t inode;
        mode_t mode;
        off_t size;
    };
    Identity identity(const fs::path& path,bool directory=false){
        plain(path);
        struct stat st{
        };
        need(lstat(path.c_str(),&st)==0&&(directory?S_ISDIR(st.st_mode):(S_ISREG(st.st_mode)&&st.st_nlink==1)),"Unsafe manager update file");
        return {
            st.st_dev,st.st_ino,st.st_mode,st.st_size
        };
    }
    void same(const fs::path& path,const Identity& before,bool directory=false){
        const auto after=identity(path,directory);
        need(before.device==after.device&&before.inode==after.inode&&before.mode==after.mode&&(directory||before.size==after.size),"Manager update storage or file identity changed");
    }
    bool helperExfat(const fs::path& path){
        auto parent=path;
        while(parent!=parent.root_path()&&!pmu_helper_directory_valid(parent.c_str()))parent=parent.parent_path();
        if(!pmu_helper_directory_valid(parent.c_str()))return false;
        int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0)return false;
        const bool valid=pmu_helper_exfat(fd);close(fd);return valid;
    }
    void owned(const fs::path& path,uid_t owner,bool directory=false){
        identity(path,directory);
        struct stat st{
        };
        need(lstat(path.c_str(),&st)==0&&st.st_uid==owner&&(!(st.st_mode&0022)||helperExfat(path)),"Unsafe manager update ownership");
    }
    std::string read(const fs::path& path,size_t limit=128*1024){
        const auto before=identity(path);
        need(before.size>=0&&static_cast<uint64_t>(before.size)<=limit,"Oversized manager update file");
        std::ifstream input(path,std::ios::binary);
        need(bool(input),"Cannot read manager update file");
        std::string result{
            std::istreambuf_iterator<char>(input),{
            }
        };
        same(path,before);
        need(result.size()==static_cast<size_t>(before.size),"Manager update file changed while reading");
        return result;
    }
    struct Directory {
        int fd=-1;
        Identity value{
        };
        explicit Directory(int descriptor):fd(descriptor){
            struct stat st{
            };
            if(fd<0||fstat(fd,&st)||!S_ISDIR(st.st_mode)){
                if(fd>=0)close(fd);
                throw std::runtime_error("Cannot pin manager update directory");
            }
            value={
                st.st_dev,st.st_ino,st.st_mode,st.st_size
            };
        }
        ~Directory(){
            if(fd>=0)close(fd);
        }
        Directory(const Directory&)=delete;
        Directory& operator=(const Directory&)=delete;
        void sync()const{
            need(fsync(fd)==0,"Cannot sync manager update directory");
        }
    };
    using DirectoryPtr=std::shared_ptr<Directory>;
    DirectoryPtr descend(int parent,const fs::path& relative){
        int fd=dup(parent);
        need(fd>=0,"Cannot retain manager update directory");
        for(const auto& part:relative){
            if(part.empty()||part==".")continue;
            need(part!=".."&&!part.is_absolute(),"Unsafe manager update directory component");
            int next=openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            close(fd);
            need(next>=0,"Manager update directory disappeared or contains a link");
            fd=next;
        }
        return std::make_shared<Directory>(fd);
    }
    DirectoryPtr pin(const fs::path& path,const Identity& expected){
        Directory root(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        const auto result=descend(root.fd,path.relative_path());
        need(result->value.device==expected.device&&result->value.inode==expected.inode&&result->value.mode==expected.mode,"Manager update directory identity changed");
        return result;
    }
    Identity fileAt(const Directory& directory,const std::string& name){
        struct stat st{
        };
        need(fstatat(directory.fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(st.st_mode)&&st.st_nlink==1,"Unsafe pinned manager update file");
        return {
            st.st_dev,st.st_ino,st.st_mode,st.st_size
        };
    }
    bool presentAt(const Directory& directory,const std::string& name){
        struct stat st{
        };
        return fstatat(directory.fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0;
    }
    void sameAt(const Directory& directory,const std::string& name,const Identity& expected){
        const auto found=fileAt(directory,name);
        need(found.device==expected.device&&found.inode==expected.inode&&found.mode==expected.mode&&found.size==expected.size,"Pinned manager update file changed");
    }
    std::string hashAt(const Directory& directory,const std::string& name){
        const auto before=fileAt(directory,name);
        const int fd=openat(directory.fd,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        need(fd>=0,"Cannot read pinned manager update file");
        std::string result;
        try{
            result=sha256("/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd));
        }
        catch(...){
            close(fd);
            throw;
        }
        close(fd);
        sameAt(directory,name,before);
        return result;
    }
    void saveAt(const Directory& directory,const std::string& name,const std::string& bytes,mode_t mode){
        int fd=openat(directory.fd,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,mode);
        need(fd>=0,"Cannot stage manager update file");
        size_t offset=0;
        bool good=true;
        while(offset<bytes.size()){
            ssize_t count=write(fd,bytes.data()+offset,bytes.size()-offset);
            if(count<0&&errno==EINTR)continue;
            if(count<=0){
                good=false;
                break;
            }
            offset+=static_cast<size_t>(count);
        }
        if(fchmod(fd,mode)||fsync(fd))good=false;
        if(close(fd))good=false;
        need(good,"Cannot finish manager update staging");
    }
    struct Stage {
        DirectoryPtr root,directory;
        std::string name;
    };
    Stage temporary(const DirectoryPtr& root){
        std::array<unsigned char,12> random{
        };
        const int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        need(fd>=0,"Cannot identify manager update transaction");
        const auto count=::read(fd,random.data(),random.size());
        close(fd);
        need(count==static_cast<ssize_t>(random.size()),"Cannot identify manager update transaction");
        const char* digits="0123456789abcdef";
        std::string name=".manager-update.";
        for(auto byte:random){
            name+=digits[byte>>4];
            name+=digits[byte&15];
        }
        need(mkdirat(root->fd,name.c_str(),0700)==0,"Cannot create manager update transaction");
        return {
            root,descend(root->fd,name),name
        };
    }
    struct Lock {
        int fd=-1;
        Lock(const fs::path& root,uid_t owner){
            fd=open((root/"manager-update.lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
            struct stat st{
            };
            if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=owner||(st.st_mode&0022)||flock(fd,LOCK_EX|LOCK_NB)){
                if(fd>=0)close(fd);
                throw std::runtime_error("Another manager update is running or its lock is unsafe");
            }
        }
        ~Lock(){
            if(fd>=0)close(fd);
        }
    };
    bool digest(const std::string& value){
        return std::regex_match(value,std::regex("[0-9a-f]{64}"));
    }
    bool runtimeName(const std::string& name){
        for(const auto* file:runtime)if(name==std::string("autorun/runtime/")+file)return true;
        return std::regex_match(name,std::regex("autorun/runtime/aarch64/(manager|shortcut|shortcut-hook\\.so)"))||std::regex_match(name,std::regex("autorun/runtime/licenses/[A-Za-z0-9][A-Za-z0-9_.-]*"));
    }
    bool runtimeForArch(const std::string& name,const std::string& arch){
        for(const auto* file:runtime)if(name=="autorun/runtime/"+managerRuntimePath(file,arch))return true;
        return std::regex_match(name,std::regex("autorun/runtime/licenses/[A-Za-z0-9][A-Za-z0-9_.-]*"));
    }
    bool bundleName(const std::string& name){
        return runtimeName(name)||name==".mpc-manager-id"||name=="autorun/autorun.check"||name=="autorun/autorun.json"||name=="autorun/open.sh"||name=="Plugin Manager helper.xpa";
    }
    Json manifest(const fs::path& path){
        const auto value=Json::parse(read(path));
        need(value.is_object()&&!value.empty()&&value.size()<=256,"Invalid manager update manifest");
        for(const auto& item:value.items())need(bundleName(item.key())&&item.value().is_string()&&digest(item.value().get<std::string>()),"Invalid manager update manifest entry");
        return value;
    }
    void inventory(const fs::path& root,const Json& files,const fs::path& subtree){
        plain(root/subtree);
        std::set<std::string> found;
        for(const auto& entry:fs::recursive_directory_iterator(root/subtree)){
            plain(entry.path());
            if(entry.is_directory())continue;
            identity(entry.path());
            const auto name=entry.path().lexically_relative(root).generic_string();
            need(files.contains(name),"Manager runtime contains unowned files; manual update required");
            found.insert(name);
        }
        for(const auto& item:files.items())if(beneath(subtree,fs::path(item.key())))need(found.count(item.key()),"Manager runtime is incomplete");
    }
    std::string runtimeManifest(const Json& files,const std::string& arch){
        std::string value;
        for(const auto* name:runtime){
            const auto relative=managerRuntimePath(name,arch);
            const std::string key="autorun/runtime/"+relative;
            need(files.contains(key),"Manager update is missing runtime files");
            value+=files.at(key).get<std::string>()+"  "+relative+'\n';
        }
        return value;
    }
    std::map<std::string,std::string> helperArchive(const fs::path& path){
        identity(path);
        need(fs::file_size(path)<=8*1024*1024,"Oversized manager helper");
        auto* ar=archive_read_new();
        need(ar!=nullptr,"Cannot inspect manager helper");
        archive_read_support_format_zip(ar);
        std::map<std::string,std::string> result;
        try{
            need(archive_read_open_filename(ar,path.c_str(),65536)==ARCHIVE_OK,"Cannot open manager helper");
            archive_entry* entry=nullptr;
            int status;
            const std::string prefix="Devices/Plugin Manager helper/";
            while((status=archive_read_next_header(ar,&entry))==ARCHIVE_OK){
                const char* raw=archive_entry_pathname(entry);
                need(raw!=nullptr,"Invalid manager helper member");
                const std::string full(raw);
                need(full.compare(0,prefix.size(),prefix)==0,"Unsupported manager helper layout");
                const auto name=full.substr(prefix.size());
                need(helperNames.count(name)&&!result.count(name)&&archive_entry_filetype(entry)==AE_IFREG&&!archive_entry_symlink(entry)&&!archive_entry_hardlink(entry)&&!archive_entry_is_encrypted(entry),"Unsupported manager helper member");
                const auto size=archive_entry_size(entry);
                need(size>0&&size<=1024*1024,"Invalid manager helper size");
                std::string data(static_cast<size_t>(size),'\0');
                size_t offset=0;
                while(offset<data.size()){
                    const auto count=archive_read_data(ar,data.data()+offset,data.size()-offset);
                    need(count>0,"Incomplete manager helper");
                    offset+=static_cast<size_t>(count);
                }
                char extra;
                need(archive_read_data(ar,&extra,1)==0,"Manager helper size mismatch");
                result.emplace(name,std::move(data));
            }
            need(status==ARCHIVE_EOF&&(result.size()==9||result.size()==10||result.size()==11||result.size()==13),"Incomplete manager helper");
            for(const auto* name:{".owner","PluginManager.xpj","bootstrap.sh","controller.ini","manager-enabled","manager-entry.so","plugin_manager.lua","runtime.sha256","sd-install-capability"})need(result.count(name),"Incomplete manager helper");
            need(bool(result.count("manager-entry-aarch64.so"))==bool(result.count("runtime-aarch64.sha256")),"Incomplete ARM64 manager helper");
            need(!result.count("bootstrap-note-aarch64")||(result.count("bootstrap-note")&&result.count("manager-entry-aarch64.so")),"Incomplete failure-note writer");
            need(!result.count("bootstrap-note")||!result.count("manager-entry-aarch64.so")||result.count("bootstrap-note-aarch64"),"Missing ARM64 failure-note writer");
            archive_read_free(ar);
            return result;
        }
        catch(...){
            archive_read_free(ar);
            throw;
        }
    }
    std::string decode(const std::string& input){
        std::string out;
        for(size_t i=0;i<input.size();++i){
            char c=input[i];
            if(c=='\\'){
                need(i+3<input.size(),"Invalid mount escape");
                const auto value=input.substr(i+1,3);
                need(value=="040"||value=="134","Unsupported mount escape");
                c=value=="040"?' ':'\\';
                i+=3;
            }
            need(static_cast<unsigned char>(c)>=32&&c!=127,"Invalid mount path");
            out+=c;
        }
        return out;
    }
    struct Mount {
        fs::path root;
        dev_t device;
        unsigned long id;
        bool writable,supported;
    };
    std::vector<Mount> mounts(const ManagerUpdatePaths& paths){
        std::ifstream input(paths.mountinfo);
        need(bool(input),"Cannot inspect manager update storage");
        std::vector<Mount> result;
        std::set<dev_t> seen;
        std::string line;
        while(std::getline(input,line)){
            need(line.size()<16384,"Invalid mount inventory");
            std::istringstream fields(line);
            unsigned long id,parent;
            std::string device,root,point,options,token,type;
            need(bool(fields>>id>>parent>>device>>root>>point>>options),"Invalid mount record");
            while(fields>>token&&token!="-"){
            }
            need(token=="-"&&bool(fields>>type),"Invalid mount filesystem");
            const fs::path location=decode(point);
            if(location.parent_path()!=paths.media||root!="/")continue;
            plain(location);
            const auto number=paths.fileDevice(location);
            need(device==std::to_string(major(number))+':'+std::to_string(minor(number)),"Manager media identity changed");
            need(seen.insert(number).second,"Manager media is mounted more than once");
            bool protectedDevice=false;
            for(const auto& item:paths.protectedRoots)if(present(item)&&paths.fileDevice(item)==number)protectedDevice=true;
            if(protectedDevice)continue;
            const auto opts=','+options+',';

            result.push_back({
                location,number,id,opts.find(",rw,")!=std::string::npos,type=="exfat"||type=="vfat"||type=="ext4"
            });
        }
        return result;
    }
    struct Target {
        fs::path root;
        Mount mount;
        Identity identity;
        bool sd;
        Json oldFiles,receipt;
    };
    struct Guard {
        Identity identity;
        std::string hash;
    };
    struct Change {
        fs::path target,root;
        DirectoryPtr parent,stage;
        std::string name,fresh,backup;
        Identity before;
        std::string oldHash,newHash;
        bool published=false,existed=true;
    };
    ManagerUpdatePaths defaults(ManagerUpdatePaths paths){
#if defined(__aarch64__)
        if(paths.helper==PMU_HELPER_DIRECTORY){
            const char* helper=std::getenv("MPC_MANAGER_SD_HELPER");
            if(helper&&pmu_helper_directory_valid(helper))paths.helper=helper;
        }
#endif
        if(!paths.fileDevice)paths.fileDevice=[](const fs::path& path){
            struct stat st{
            };
            need(stat(path.c_str(),&st)==0,"Manager update media is unavailable");
            return st.st_dev;
        };
        if(!paths.blockDevice)paths.blockDevice=[](const fs::path& path){
            struct stat st{
            };
            need(stat(path.c_str(),&st)==0&&S_ISBLK(st.st_mode),"Configured SD identity is unavailable");
            return st.st_rdev;
        };
        if(!paths.requireStopped)paths.requireStopped=[](){
            need(runProcess({
                "systemctl","is-active","--quiet","acvs.service"
            })==3,"Return to the manager before applying its update");
        };
        return paths;
    }
}
Json applyManagerUpdate(const fs::path& source,const fs::path& session,const Json& staged,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount){
    return applyManagerUpdate(source,session,staged,ManagerUpdatePaths{
    },progress,amount);
}
Json applyManagerUpdate(const fs::path& source,const fs::path& session,const Json& staged,ManagerUpdatePaths paths,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount){
    need(staged.is_object()&&staged.contains("release")&&staged.at("release").is_object(),"Invalid staged manager release");
    paths=defaults(std::move(paths));
    need(geteuid()==paths.owner,"Manager update requires its native service");
    plain(source);
    owned(session,paths.owner,true);
    owned(paths.state,paths.owner);
    paths.requireStopped();
    const bool hasHelper=present(paths.helper);
    if(hasHelper)owned(paths.helper,paths.owner,true);
    if(hasHelper)need(sha256(paths.firmware)==paths.firmwareHash,"Unsupported update: this firmware requires a manual manager update");
    const auto state=read(paths.state,512);
    std::istringstream stateInput(state);
    std::string baseName,sourceName,extra;
    need(bool(std::getline(stateInput,baseName))&&bool(std::getline(stateInput,sourceName))&&!std::getline(stateInput,extra)&&sourceName==source.string(),"Manager update does not own the current session");
    const fs::path base=baseName;
    need(base.parent_path()==paths.ram&&std::regex_match(base.filename().string(),std::regex("mpc-usb-session\\.[A-Za-z0-9]{6}")),"Invalid manager RAM session");
    owned(base,paths.owner,true);
    Lock lock(base,paths.owner);
    const auto baseIdentity=identity(base,true),helperIdentity=hasHelper?identity(paths.helper,true):Identity{
    },stateIdentity=identity(paths.state);
    const fs::path bundle=staged.at("bundle_root").get<std::string>();
    need(beneath(session,bundle),"Manager update staging is outside its session");
    plain(bundle);
    const auto files=manifest(bundle/"USB-SHA256.json");
    need(files==staged.at("files"),"Manager update inventory changed");
    need(!hasHelper||files.contains("Plugin Manager helper.xpa"),"Unsupported update: a matching universal helper package is required");
    for(const auto& item:files.items()){
        identity(bundle/item.key());
        need(fs::file_size(bundle/item.key())<=64*1024*1024&&sha256(bundle/item.key())==item.value(),"Staged manager update checksum changed");
    }
    inventory(bundle,files,"autorun");
    need(read(bundle/".mpc-manager-id",64)==marker,"Invalid manager update marker");
    const auto helper=files.contains("Plugin Manager helper.xpa")?helperArchive(bundle/"Plugin Manager helper.xpa"):std::map<std::string,std::string>{
    };
    const auto manifestName=managerRuntimeManifest(paths.arch);
    const auto nextManifest=runtimeManifest(files,paths.arch);
    if(!helper.empty()){
        need(helper.at("manager-enabled")=="open-mpc-manager-reusable-v1\n"&&helper.at("sd-install-capability")=="open-mpc-manager-sd-v1\n","Unsupported manager helper capability");
        need(helper.count(manifestName)&&helper.at(manifestName)==nextManifest,"Manager helper and update runtime differ");
    }
    for(const auto& item:helper)if(hasHelper&&item.first!="runtime.sha256"&&item.first!="runtime-aarch64.sha256"&&item.first!="PluginManager.xpj"){
        need(present(paths.helper/item.first),"Unsupported update: import the new Plugin Manager helper.xpa manually");
        owned(paths.helper/item.first,paths.owner);
        need(read(paths.helper/item.first,1024*1024)==item.second,"Unsupported update: changed native helper requires a manual update");
    }
    if(hasHelper)owned(paths.helper/manifestName,paths.owner);
    const auto oldManifest=hasHelper?read(paths.helper/manifestName,512):runtimeManifest(manifest(source/"USB-SHA256.json"),paths.arch);
    std::map<std::string,std::string> oldRuntime;
    std::istringstream manifestInput(oldManifest);
    for(const auto* name:runtime){
        std::string line;
        need(bool(std::getline(manifestInput,line))&&line.size()==66+managerRuntimePath(name,paths.arch).size()&&line.substr(64)==std::string("  ")+managerRuntimePath(name,paths.arch)&&digest(line.substr(0,64)),"Invalid installed helper runtime manifest");
        oldRuntime[name]=line.substr(0,64);
    }
    need(!std::getline(manifestInput,extra),"Unexpected helper runtime manifest entry");
    for(const auto* name:runtime){
        owned(base/name,paths.owner);
        need(sha256(base/name)==oldRuntime.at(name),"Cached manager runtime differs from its helper");
        if(std::string(name)!="manager"&&std::string(name)!="font.ttf")need(files.at(std::string("autorun/runtime/")+managerRuntimePath(name,paths.arch))==oldRuntime.at(name),"Unsupported update: changed launcher or supervisor requires a manual update");
    }
    std::string configuration,uuid;
    const auto configPath=paths.helper/"installed-storage";
    need(!hasHelper||!present(paths.helper/"installed-storage.removing"),"Finish the interrupted SD removal before updating");
    if(hasHelper&&present(configPath)){
        owned(configPath,paths.owner);
        need((identity(configPath).mode&07777)==0600||helperExfat(configPath),"Unsafe configured SD selection");
        configuration=read(configPath,160);
        const std::string prefix="open-mpc-manager-storage-v1\n";
        need(configuration.compare(0,prefix.size(),prefix)==0&&configuration.back()=='\n',"Invalid configured SD selection");
        uuid=configuration.substr(prefix.size(),configuration.size()-prefix.size()-1);
        need(std::regex_match(uuid,std::regex("[A-Fa-f0-9-]{1,64}")),"Invalid configured SD identity");
    }
    const auto mounted=mounts(paths);
    std::vector<Target> targets;
    bool current=false,configuredPresent=false;
    unsigned usbCount=0;
    dev_t configuredDevice=0;
    if(!uuid.empty()&&present(paths.uuids/uuid))configuredDevice=paths.blockDevice(paths.uuids/uuid);
    for(const auto& volume:mounted){
        std::vector<std::pair<fs::path,bool>> candidates;
        if(!uuid.empty()&&!configuredDevice&&present(volume.root/"Plugin Manager USB/.mpc-manager-id"))throw std::runtime_error("Mounted SD manager identity is unavailable; reconnect the card before updating");
        if(present(volume.root/".mpc-manager-id")){
            need(++usbCount==1,"Multiple manager USB sources require a manual update");
            candidates.push_back({
                volume.root,false
            });
        }
        if(configuredDevice&&volume.device==configuredDevice){
            need(!configuredPresent,"Configured SD has ambiguous mounts");
            configuredPresent=true;
            candidates.push_back({
                volume.root/"Plugin Manager USB",true
            });
        }
        for(const auto& candidate:candidates){
            need(volume.writable&&volume.supported,"A mounted manager source is read-only or uses an unsupported filesystem");
            Target target{
                candidate.first,volume,identity(candidate.first,true),candidate.second,Json::object(),Json::object()
            };
            need(read(target.root/".mpc-manager-id",64)==marker,"Invalid manager source marker");
            if(target.sd){
                target.receipt=Json::parse(read(target.root/".manager-install.json"));
                need(target.receipt.is_object()&&target.receipt.size()==5&&target.receipt.value("schema",0)==1&&target.receipt.value("owner","")=="plugin-manager-usb"&&target.receipt.value("uuid","")==uuid&&target.receipt.at("installed")==true&&target.receipt.at("files").is_object(),"Unowned SD manager cannot be updated");
                target.oldFiles=target.receipt.at("files");
            }
            else target.oldFiles=manifest(target.root/"USB-SHA256.json");
            Json expected=files;
            if(target.sd){
                expected=Json::object();
                for(const auto& item:files.items())if(runtimeForArch(item.key(),paths.arch)||item.key()==".mpc-manager-id")expected[item.key()]=item.value();
            }
            need(target.oldFiles.size()==expected.size()||(!target.sd&&files.contains("Plugin Manager helper.xpa")&&!target.oldFiles.contains("Plugin Manager helper.xpa")&&target.oldFiles.size()+1==expected.size()),"Unsupported update: managed file layout changed");
            for(const auto& item:expected.items()){
                if(!target.oldFiles.contains(item.key())&&item.key()=="Plugin Manager helper.xpa"&&!target.sd){
                    need(!present(target.root/item.key()),"Unowned helper archive must be preserved");
                    continue;
                }
                need(target.oldFiles.contains(item.key())&&target.oldFiles.at(item.key()).is_string()&&digest(target.oldFiles.at(item.key()).get<std::string>()),"Unsupported update: managed file layout changed");
                identity(target.root/item.key());
                need(sha256(target.root/item.key())==target.oldFiles.at(item.key()),"Existing manager files were modified; manual update required");
            }
            for(const auto* name:runtime)need(target.oldFiles.at(std::string("autorun/runtime/")+managerRuntimePath(name,paths.arch))==oldRuntime.at(name),"Another installed manager uses a different runtime; manual update required");
            inventory(target.root,target.oldFiles,target.sd?"autorun/runtime":"autorun");
            if(target.root==source)current=true;
            targets.push_back(std::move(target));
        }
    }
    need(current,"Current manager source is absent, read-only or not owned");
    std::map<fs::path,Stage> stages;
    std::map<fs::path,Identity> rootIdentities;
    std::map<fs::path,DirectoryPtr> roots;
    std::vector<Change> changes;
    Json updated=Json::array();
    rootIdentities[base]=baseIdentity;
    if(hasHelper)rootIdentities[paths.helper]=helperIdentity;
    for(const auto& target:targets)rootIdentities[target.root]=target.identity;
    for(const auto& item:rootIdentities)roots[item.first]=pin(item.first,item.second);
    std::map<fs::path,Guard> guards;
    auto guard=[&](const fs::path& path,const std::string& hash){
        guards[path]={
            identity(path),hash
        };
    };
    for(const auto& target:targets){
        for(const auto& item:target.oldFiles.items())guard(target.root/item.key(),item.value());
        const auto metadata=target.root/(target.sd?".manager-install.json":"USB-SHA256.json");
        guard(metadata,sha256(metadata));
    }
    for(const auto* name:runtime)guard(base/name,oldRuntime.at(name));
    if(hasHelper)for(const auto& item:helper)if(item.first!="PluginManager.xpj")guard(paths.helper/item.first,sha256(paths.helper/item.first));
    Json cachedLicenses=Json::object();
    if(present(base/"licenses")){
        const auto currentTarget=std::find_if(targets.begin(),targets.end(),[&](const Target& target){
            return target.root==source;
        });
        for(const auto& item:currentTarget->oldFiles.items())if(item.key().rfind("autorun/runtime/licenses/",0)==0){
            const auto relative=item.key().substr(16);
            cachedLicenses[relative]=item.value();
            guard(base/relative,item.value());
        }
        inventory(base,cachedLicenses,"licenses");
    }
    auto recheck=[&](){
        paths.requireStopped();
        same(base,baseIdentity,true);
        if(hasHelper)same(paths.helper,helperIdentity,true);
        else need(!present(paths.helper),"Manager helper appeared during update");
        same(paths.state,stateIdentity);
        need(read(paths.state,512)==state,"Manager session changed");
        need((present(configPath)?read(configPath,160):std::string())==configuration,"Configured SD selection changed");
        for(const auto& item:guards){
            same(item.first,item.second.identity);
            need(sha256(item.first)==item.second.hash,"Manager file changed during update");
        }
        const auto now=mounts(paths);
        for(const auto& target:targets){
            same(target.root,target.identity,true);
            need(std::any_of(now.begin(),now.end(),[&](const Mount& value){
                return value.root==target.mount.root&&value.device==target.mount.device&&value.id==target.mount.id&&value.writable&&value.supported;
            }),"Manager media was removed or replaced");
            if(target.sd)need(paths.blockDevice(paths.uuids/uuid)==target.mount.device,"Configured SD identity changed");
        }
    };
    auto add=[&](const fs::path& root,const fs::path& relative,const std::string& bytes,const std::string& expected=std::string()){
        const auto path=root/relative;
        const bool existed=guards.count(path);
        Identity before{
        };
        std::string old;
        const auto parent=descend(roots.at(root)->fd,relative.parent_path());
        const auto name=relative.filename().string();
        if(existed){
            before=identity(path);
            sameAt(*parent,name,guards.at(path).identity);
            need(hashAt(*parent,name)==guards.at(path).hash,"Manager changed during staging");
            old=read(path,64*1024*1024);
        }
        else need(relative=="Plugin Manager helper.xpa"&&!presentAt(*parent,name),"Unowned manager update destination must be preserved");
        if(existed&&old==bytes){
            need(expected.empty()||expected==guards.at(path).hash,"Staged update checksum changed");
            return;
        }
        need(fs::space(root).available>=old.size()+bytes.size()+1024*1024,"Insufficient space for a reversible manager update");
        same(root,rootIdentities.at(root),true);
        if(!stages.count(root))stages[root]=temporary(roots.at(root));
        const auto directory=stages.at(root).directory;
        const auto index=std::to_string(changes.size());
        const auto backup=index+".old",fresh=index+".new";
        const mode_t mode=existed?before.mode&0777:0644;
        if(existed){
            saveAt(*directory,backup,old,mode);
        }
        saveAt(*directory,fresh,bytes,mode);
        const auto newHash=hashAt(*directory,fresh);
        need((!existed||hashAt(*directory,backup)==guards.at(path).hash)&&(expected.empty()||newHash==expected),"Manager staging checksum changed");
        changes.push_back({
            path,root,parent,directory,name,fresh,backup,before,existed?guards.at(path).hash:std::string(),newHash,false,existed
        });
    };
    auto cleanup=[&](){
        for(const auto& item:stages){
            const auto& stage=item.second;
            try{
                for(const auto& change:changes)if(change.root==item.first){
                    unlinkat(stage.directory->fd,change.fresh.c_str(),0);
                    unlinkat(stage.directory->fd,change.backup.c_str(),0);
                }
                unlinkat(stage.directory->fd,"transaction.json",0);
                stage.directory->sync();
                struct stat st{
                };
                need(fstatat(stage.root->fd,stage.name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0&&st.st_dev==stage.directory->value.device&&st.st_ino==stage.directory->value.inode,"Transaction directory changed");
                need(unlinkat(stage.root->fd,stage.name.c_str(),AT_REMOVEDIR)==0,"Cannot remove manager update transaction");
                stage.root->sync();
            }
            catch(...){
            }
        }
    };
    auto checkTarget=[&](const Change& change){
        if(change.existed){
            sameAt(*change.parent,change.name,change.before);
            need(hashAt(*change.parent,change.name)==change.oldHash,"Manager changed before update publication");
        }
        else need(!presentAt(*change.parent,change.name),"Manager update destination appeared");
    };
    auto rollbackStorage=[&](const fs::path& root){
        paths.requireStopped();
        same(root,rootIdentities.at(root),true);
        for(const auto& target:targets)if(target.root==root){
            const auto now=mounts(paths);
            need(std::any_of(now.begin(),now.end(),[&](const Mount& item){
                return item.root==target.mount.root&&item.device==target.mount.device&&item.id==target.mount.id&&item.writable&&item.supported;
            }),"Manager storage is unavailable for rollback");
        }
    };
    try{
        if(progress)progress("Preparing a verified manager update...");
        if(amount)amount(0);
        for(const auto& target:targets){
            Json after=files;
            if(target.sd){
                after=Json::object();
                for(const auto& item:files.items())if(runtimeForArch(item.key(),paths.arch)||item.key()==".mpc-manager-id")after[item.key()]=item.value();
            }
            for(const auto& item:after.items()){
                add(target.root,item.key(),read(bundle/item.key(),64*1024*1024),files.at(item.key()));
            }
            if(target.sd){
                auto receipt=target.receipt;
                receipt["files"]=after;
                add(target.root,".manager-install.json",receipt.dump(2)+'\n');
            }
            else add(target.root,"USB-SHA256.json",files.dump(2)+'\n');
            updated.push_back(target.root.string());
        }
        if(hasHelper){
            add(paths.helper,manifestName,nextManifest);
            const std::string other=paths.arch=="aarch64"?"armv7":"aarch64";
            const auto otherName=managerRuntimeManifest(other);
            if(helper.count(otherName))add(paths.helper,otherName,runtimeManifest(files,other));
        }
        add(base,"manager",read(bundle/"autorun/runtime"/managerRuntimePath("manager",paths.arch),64*1024*1024),files.at("autorun/runtime/"+managerRuntimePath("manager",paths.arch)));
        add(base,"font.ttf",read(bundle/"autorun/runtime/font.ttf",64*1024*1024),files.at("autorun/runtime/font.ttf"));
        for(const auto& item:cachedLicenses.items())add(base,item.key(),read(bundle/"autorun/runtime"/item.key(),1024*1024),files.at("autorun/runtime/"+item.key()));
        recheck();
        for(const auto& change:changes){
            checkTarget(change);
            change.stage->sync();
        }
        for(const auto& item:stages){
            Json journal=Json::array();
            for(const auto& change:changes)if(change.root==item.first)journal.push_back({
                {
                    "path",change.target.lexically_relative(change.root).generic_string()
                },{
                    "old",change.backup
                },{
                    "new",change.fresh
                },{
                    "old_sha256",change.oldHash
                },{
                    "new_sha256",change.newHash
                },{
                    "existed",change.existed
                }
            });
            saveAt(*item.second.directory,"transaction.json",Json({
                {
                    "schema",1
                },{
                    "files",journal
                }
            }).dump(2)+'\n',0600);
            item.second.directory->sync();
            item.second.root->sync();
        }
        if(progress)progress("Installing the manager update...");
        for(size_t i=0;i<changes.size();++i){
            auto& change=changes[i];
            recheck();
            checkTarget(change);
            if(paths.beforePublish)paths.beforePublish(i);
            recheck();
            checkTarget(change);
            need(hashAt(*change.stage,change.fresh)==change.newHash,"Staged manager replacement changed");
            if(paths.beforeRename)paths.beforeRename(i);
            need(renameat(change.stage->fd,change.fresh.c_str(),change.parent->fd,change.name.c_str())==0,"Cannot publish manager update");
            change.published=true;
            guards[change.target]={
                fileAt(*change.parent,change.name),change.newHash
            };
            change.parent->sync();
            need(hashAt(*change.parent,change.name)==change.newHash,"Manager update readback failed");
            if(amount)amount(float(i+1)/float(changes.size()));
        }
        recheck();
        for(const auto& change:changes)need(hashAt(*change.parent,change.name)==change.newHash,"Manager update final verification failed");
    }
    catch(...){
        const auto failure=std::current_exception();
        bool restored=true;
        for(auto it=changes.rbegin();it!=changes.rend();++it)if(it->published){
            try{
                rollbackStorage(it->root);
                need(hashAt(*it->parent,it->name)==it->newHash&&(!it->existed||hashAt(*it->stage,it->backup)==it->oldHash),"Changed manager file cannot be rolled back");
                if(it->existed){
                    need(renameat(it->stage->fd,it->backup.c_str(),it->parent->fd,it->name.c_str())==0,"Cannot restore manager update backup");
                    need(hashAt(*it->parent,it->name)==it->oldHash,"Manager rollback readback failed");
                }
                else{
                    need(unlinkat(it->parent->fd,it->name.c_str(),0)==0&&!presentAt(*it->parent,it->name),"Manager rollback removal failed");
                }
                it->parent->sync();
            }
            catch(...){
                restored=false;
            }
        }
        if(restored){
            cleanup();
        }
        else{
            throw std::runtime_error("Manager update failed and rollback could not be verified; recovery files were preserved. Keep this session open and do not restart MPC.");
        }
        std::rethrow_exception(failure);
    }
    cleanup();
    if(amount)amount(1);
    const bool absent=!uuid.empty()&&!configuredPresent;
    const std::string message="Manager update installed. Return to MPC; the next shortcut opening uses the updated manager."+(absent?std::string(" The configured SD card was absent and was not updated."):std::string())+" Disconnected manager media was not updated.";
    if(progress)progress(message);
    return {
        {
            "applied",true
        },{
            "ram_refreshed",true
        },{
            "updated_targets",updated
        },{
            "source_on_sd",source.parent_path()!=paths.media
        },{
            "activation","next_open"
        },{
            "configured_sd_absent",absent
        },{
            "message",message
        },{
            "release",staged.at("release")
        }
    };
}
