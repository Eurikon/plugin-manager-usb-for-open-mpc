#pragma once
#include "filesystem_identity.hpp"
#include <array>
#include <dlfcn.h>
#include <openssl/evp.h>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pmu {
class PluginTransaction {
    using Path=std::filesystem::path;
    struct Entry {
        std::filesystem::file_type type;
        std::filesystem::perms permissions;
        std::filesystem::file_time_type modified;
        std::string content;
        uintmax_t bytes;
        uid_t owner;
        gid_t group;
        bool operator==(const Entry& other)const{return type==other.type&&permissions==other.permissions&&modified==other.modified&&content==other.content&&bytes==other.bytes&&owner==other.owner&&group==other.group;}
    };
    using Inventory=std::map<Path,Entry>;
    struct Pin {
        int descriptor=-1;
        struct stat identity{};
        std::string mount;
        explicit Pin(const Path& path){
            descriptor=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            try{need(descriptor>=0&&fstat(descriptor,&identity)==0,"Cannot pin plugin recovery directory");mount=mountIdentity(descriptor);}catch(...){if(descriptor>=0)close(descriptor);throw;}
        }
        ~Pin(){if(descriptor>=0)close(descriptor);}
        void check(const Path& path)const{
            struct stat current{};
            need(std::filesystem::canonical(path)==path&&lstat(path.c_str(),&current)==0&&S_ISDIR(current.st_mode)&&current.st_dev==identity.st_dev&&current.st_ino==identity.st_ino,"Plugin recovery drive or directory changed");
            const Pin fresh(path);need(fresh.mount==mount,"Plugin recovery mount changed");
        }
    };
    struct Snapshot {Path original,container;std::shared_ptr<Pin> parent,backup;Inventory entries;};
    std::vector<Snapshot> snapshots;
    bool preserve=false,finished=false;
    static void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
    static bool present(const Path& path){return std::filesystem::symlink_status(path).type()!=std::filesystem::file_type::not_found;}
    static uintmax_t add(uintmax_t left,uintmax_t right){need(right<=std::numeric_limits<uintmax_t>::max()-left,"Plugin recovery size overflow");return left+right;}
    struct HashApi {
        void* library=nullptr;
        decltype(&EVP_MD_CTX_new) create=nullptr;
        decltype(&EVP_MD_CTX_free) release=nullptr;
        decltype(&EVP_sha256) algorithm=nullptr;
        decltype(&EVP_DigestInit_ex) initialize=nullptr;
        decltype(&EVP_DigestUpdate) update=nullptr;
        decltype(&EVP_DigestFinal_ex) finish=nullptr;
        HashApi(){
            library=dlopen("libcrypto.so.3",RTLD_NOW|RTLD_LOCAL);if(!library)library=dlopen("libcrypto.so.1.1",RTLD_NOW|RTLD_LOCAL);need(library!=nullptr,"Plugin recovery checksum library is unavailable");
            create=reinterpret_cast<decltype(create)>(dlsym(library,"EVP_MD_CTX_new"));release=reinterpret_cast<decltype(release)>(dlsym(library,"EVP_MD_CTX_free"));algorithm=reinterpret_cast<decltype(algorithm)>(dlsym(library,"EVP_sha256"));initialize=reinterpret_cast<decltype(initialize)>(dlsym(library,"EVP_DigestInit_ex"));update=reinterpret_cast<decltype(update)>(dlsym(library,"EVP_DigestUpdate"));finish=reinterpret_cast<decltype(finish)>(dlsym(library,"EVP_DigestFinal_ex"));
            if(!create||!release||!algorithm||!initialize||!update||!finish){dlclose(library);library=nullptr;throw std::runtime_error("Plugin recovery checksum API is unavailable");}
        }
        ~HashApi(){if(library)dlclose(library);}
    };
    static std::string digest(const Path& path){
        static thread_local HashApi api;
        struct File {int descriptor;~File(){if(descriptor>=0)close(descriptor);}} file{open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
        struct stat before{},after{};need(file.descriptor>=0&&fstat(file.descriptor,&before)==0&&S_ISREG(before.st_mode),"Cannot read plugin recovery file");
        std::unique_ptr<EVP_MD_CTX,decltype(api.release)> context(api.create(),api.release);need(bool(context)&&api.initialize(context.get(),api.algorithm(),nullptr)==1,"Cannot initialize plugin recovery checksum");
        std::array<unsigned char,65536> block{};
        for(;;){const auto count=read(file.descriptor,block.data(),block.size());if(count<0&&errno==EINTR)continue;need(count>=0,"Cannot read plugin recovery file");if(!count)break;need(api.update(context.get(),block.data(),size_t(count))==1,"Cannot checksum plugin recovery file");}
        need(fstat(file.descriptor,&after)==0&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"Plugin file changed during recovery checksum");
        std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{};unsigned size=0;need(api.finish(context.get(),bytes.data(),&size)==1&&size==32,"Cannot finish plugin recovery checksum");
        std::string result;const char* digits="0123456789abcdef";for(unsigned i=0;i<size;++i){result+=digits[bytes[i]>>4];result+=digits[bytes[i]&15];}return result;
    }
    static Inventory inspect(const Path& root){
        Inventory result;
        if(!present(root))return result;
        auto append=[&](const Path& path){
            need(result.size()<100000,"Plugin recovery contains too many files");
            const auto status=std::filesystem::symlink_status(path);const auto type=status.type();
            need(type==std::filesystem::file_type::regular||type==std::filesystem::file_type::directory||type==std::filesystem::file_type::symlink,"Unsupported file in plugin recovery");
            struct stat identity{};need(lstat(path.c_str(),&identity)==0,"Cannot inspect plugin recovery ownership");
            Entry entry{type,status.permissions(),{},"",0,identity.st_uid,identity.st_gid};
            if(type==std::filesystem::file_type::regular){entry.content=digest(path);entry.bytes=std::filesystem::file_size(path);entry.modified=std::filesystem::last_write_time(path);}
            else if(type==std::filesystem::file_type::symlink)entry.content=std::filesystem::read_symlink(path).string();
            result.emplace(path==root?Path():path.lexically_relative(root),std::move(entry));
        };
        append(root);
        if(std::filesystem::is_directory(std::filesystem::symlink_status(root)))for(const auto& entry:std::filesystem::recursive_directory_iterator(root))append(entry.path());
        return result;
    }
    static uintmax_t allocation(const Inventory& entries){uintmax_t total=0;for(const auto& entry:entries)total=add(total,add(entry.second.bytes,4096));return total;}
    static Path temporary(const Path& parent){
        std::string pattern=(parent/".pmu-plugin-recovery-XXXXXX").string();std::vector<char> bytes(pattern.begin(),pattern.end());bytes.push_back(0);
        const auto name=mkdtemp(bytes.data());need(name!=nullptr,"Cannot create plugin recovery folder");return Path(name);
    }
    static void copy(const Path& original,const Path& saved,const Inventory& entries){
        for(const auto& pair:entries){
            const auto source=pair.first.empty()?original:original/pair.first;const auto target=pair.first.empty()?saved:saved/pair.first;const auto& entry=pair.second;
            if(entry.type==std::filesystem::file_type::directory)std::filesystem::create_directory(target);
            else if(entry.type==std::filesystem::file_type::symlink)std::filesystem::create_symlink(entry.content,target);
            else {std::filesystem::copy_file(source,target);std::filesystem::last_write_time(target,entry.modified);}
            struct stat identity{};need(lstat(target.c_str(),&identity)==0,"Cannot inspect recovery copy ownership");
            if(identity.st_uid!=entry.owner||identity.st_gid!=entry.group)need(lchown(target.c_str(),entry.owner,entry.group)==0,"Cannot preserve plugin file ownership");
            if(entry.type==std::filesystem::file_type::regular)std::filesystem::permissions(target,entry.permissions);
        }
        for(auto it=entries.rbegin();it!=entries.rend();++it)if(it->second.type==std::filesystem::file_type::directory)std::filesystem::permissions(it->first.empty()?saved:saved/it->first,it->second.permissions);
    }
    void check(const Snapshot& snapshot)const{snapshot.parent->check(snapshot.original.parent_path());snapshot.backup->check(snapshot.container);}
    void clean()noexcept{for(const auto& snapshot:snapshots)if(snapshot.backup)try{check(snapshot);std::filesystem::remove_all(snapshot.container);}catch(...){} }
    void verifiedCleanup(){
        for(const auto& snapshot:snapshots)try{check(snapshot);std::filesystem::remove_all(snapshot.container);}catch(const std::exception& error){throw std::runtime_error(std::string("Recovery cleanup failed; keep folder ")+snapshot.container.string()+": "+error.what());}
        preserve=false;snapshots.clear();
    }
public:
    PluginTransaction()=default;
    PluginTransaction(const PluginTransaction&)=delete;
    PluginTransaction& operator=(const PluginTransaction&)=delete;
    ~PluginTransaction(){if(!preserve)clean();}
    bool committed()const{return finished;}
    static uintmax_t treeAllocation(const Path& path){
        if(!present(path))return 0;
        auto bytes=[](const Path& value){const auto status=std::filesystem::symlink_status(value);return std::filesystem::is_regular_file(status)?add(std::filesystem::file_size(value),4096):uintmax_t(4096);};
        auto total=bytes(path);if(std::filesystem::is_directory(std::filesystem::symlink_status(path)))for(const auto& entry:std::filesystem::recursive_directory_iterator(path))total=add(total,bytes(entry.path()));return total;
    }
    void prepare(const std::vector<Path>& originals,const Path& destination,uintmax_t extraBytes,bool requiresLinks){
        need(snapshots.empty(),"Plugin recovery already prepared");
        need(destination.is_absolute(),"Plugin installation destination must be absolute");
        std::map<dev_t,std::pair<Path,uintmax_t>> space;
        auto reserve=[&](const Path& parent,uintmax_t bytes){struct stat status{};need(stat(parent.c_str(),&status)==0,"Cannot inspect plugin drive");auto& entry=space[status.st_dev];entry.first=parent;entry.second=add(entry.second,bytes);};
        for(const auto& path:originals){
            need(path.is_absolute()&&!path.filename().empty()&&path.filename()!="."&&path.filename()!="..","Invalid plugin recovery path");
            const auto original=std::filesystem::canonical(path.parent_path())/path.filename();
            for(const auto& other:snapshots){const auto relative=original.lexically_relative(other.original),reverse=other.original.lexically_relative(original);need((relative.empty()||*relative.begin()=="..")&&(reverse.empty()||*reverse.begin()==".."),"Overlapping plugin recovery paths");}
            need(!std::filesystem::is_symlink(std::filesystem::symlink_status(original)),"Plugin recovery refuses a linked target");
            Snapshot snapshot{original,{},std::make_shared<Pin>(original.parent_path()),{},inspect(original)};
            reserve(original.parent_path(),allocation(snapshot.entries));snapshots.push_back(std::move(snapshot));
        }
        auto parent=destination;while(!std::filesystem::exists(parent))parent=parent.parent_path();parent=std::filesystem::canonical(parent);
        reserve(parent,extraBytes);
        for(const auto& entry:space)need(std::filesystem::space(entry.second.first).available>=add(entry.second.second,8*1024*1024),"Not enough free space for plugin installation and verified recovery");
        try{
            if(requiresLinks){const auto probe=temporary(parent);try{std::filesystem::create_symlink("target",probe/"link");need(std::filesystem::read_symlink(probe/"link")=="target","Plugin drive does not support required symbolic links");std::filesystem::remove_all(probe);}catch(...){std::error_code ignored;std::filesystem::remove_all(probe,ignored);throw std::runtime_error("Plugin drive cannot create the symbolic links this package needs");}}
            for(auto& snapshot:snapshots){
                snapshot.parent->check(snapshot.original.parent_path());snapshot.container=temporary(snapshot.original.parent_path());snapshot.backup=std::make_shared<Pin>(snapshot.container);
                std::ofstream receipt(snapshot.container/"original-path");receipt<<snapshot.original.string()<<'\n';receipt.close();need(bool(receipt),"Cannot save plugin recovery path");
                copy(snapshot.original,snapshot.container/"saved",snapshot.entries);need(inspect(snapshot.original)==snapshot.entries&&inspect(snapshot.container/"saved")==snapshot.entries,"Plugin recovery copy verification failed");
            }
        }catch(...){clean();throw;}
    }
    void run(const std::function<void()>& operation){
        need(!snapshots.empty(),"Plugin recovery was not prepared");
        for(const auto& snapshot:snapshots){check(snapshot);need(inspect(snapshot.original)==snapshot.entries&&inspect(snapshot.container/"saved")==snapshot.entries,"Plugin files or recovery copies changed before installation");}
        preserve=true;
        bool failed=false;std::string cause;
        try{operation();}catch(const std::exception& error){failed=true;cause=error.what();}
        if(!failed){finished=true;verifiedCleanup();return;}
        bool incomplete=false;std::string recoveryFailure;
        try{
            // Validate every recovery copy before replacing any changed content.
            for(const auto& snapshot:snapshots){check(snapshot);need(inspect(snapshot.container/"saved")==snapshot.entries,"Plugin recovery copy changed");}
            for(auto& snapshot:snapshots){
                if(inspect(snapshot.original)==snapshot.entries)continue;
                if(present(snapshot.original))std::filesystem::rename(snapshot.original,snapshot.container/"failed");
                if(!snapshot.entries.empty())std::filesystem::rename(snapshot.container/"saved",snapshot.original);
                need(inspect(snapshot.original)==snapshot.entries,"Plugin restoration verification failed");
            }
            verifiedCleanup();
        }catch(const std::exception& recovery){incomplete=true;recoveryFailure=recovery.what();}
        if(incomplete){
            std::string paths;for(const auto& snapshot:snapshots)paths+=" "+snapshot.container.string();
            throw std::runtime_error(cause+"; recovery incomplete: "+recoveryFailure+". Keep recovery folders:"+paths);
        }
        throw std::runtime_error(cause+"; previous plugin files, settings and installation records restored and verified");
    }
};
}
