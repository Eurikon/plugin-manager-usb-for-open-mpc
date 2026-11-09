#include "native_cache.hpp"
#include "filesystem_identity.hpp"
#include <iostream>
#include <sys/mount.h>

namespace fs=std::filesystem;
static int checks=0;
static void check(bool value,const char* label){if(!value)throw std::runtime_error(label);++checks;std::cout<<"PASS: "<<label<<'\n';}
template<class Action> static bool refused(Action action){try{action();return false;}catch(const std::exception&){return true;}}
static void save(const fs::path& path,const std::string& text="image"){std::ofstream file(path);file<<text;if(!file)throw std::runtime_error("Cannot write cache fixture");}
int main(){
    char pattern[]="/tmp/pmu-native-cache-XXXXXX";const auto made=mkdtemp(pattern);if(!made)return 1;const fs::path root=made,ram=root/"ram",disk=root/"disk",cache=disk/"filmstrips";
    bool mounted=false;
    try{
        fs::create_directory(ram);fs::create_directory(disk);fs::create_directory(cache);
        check(refused([&]{pmu::NativeCache(ram,cache,[]{return true;}).validate();}),"disk-backed scratch is rejected");
        if(mount("tmpfs",ram.c_str(),"tmpfs",MS_NODEV|MS_NOSUID,"size=16m"))throw std::runtime_error("RAM fixture mount failed");
        mounted=true;
        pmu::NativeCache native(ram,cache,[]{return true;});
        native.validate();check(fs::is_directory(ram/"filmstrips"),"native cache prepares verified RAM storage");
        pmu::NativeCache::requireRam(ram);check(true,"RAM scratch is accepted");
        save(cache/"temp_18e6325a58ab845c.img");save(ram/"filmstrips/temp_18e6325a58ab845d.img");save(cache/"keep.img");save(cache/"temp_nothex.img");save(root/"settings","settings");
        fs::create_directory(cache/"temp_18e6325a58ab845e.img");fs::create_symlink(root/"settings",cache/"temp_18e6325a58ab845f.img");
        fs::create_hard_link(root/"settings",cache/"temp_18e6325a58ab8460.img");
        check(refused([&]{pmu::NativeCache(ram,cache,[]{return false;}).clean();})&&fs::exists(cache/"temp_18e6325a58ab845c.img"),"running MPC prevents all cleanup");
        check(native.clean()==2,"only recognized inactive regular cache images are removed");
        check(fs::exists(cache/"keep.img")&&fs::exists(cache/"temp_nothex.img")&&fs::is_directory(cache/"temp_18e6325a58ab845e.img")&&fs::is_symlink(cache/"temp_18e6325a58ab845f.img")&&fs::exists(cache/"temp_18e6325a58ab8460.img")&&fs::file_size(root/"settings")==8,"unrelated files, links, directories and settings are preserved");
        check(native.clean()==0,"repeated cleanup is harmless");
        const auto alias=root/"alias";fs::create_directory_symlink(ram,alias);pmu::NativeCache shared(ram,alias/"filmstrips",[]{return true;});save(ram/"filmstrips/temp_18e6325a58ab8461.img");
        check(shared.clean()==1,"a stock RAM-backed alias is cleaned only once");
        const auto linked=disk/"linked";fs::create_directory(linked);fs::create_directory_symlink(cache,linked/"filmstrips");
        check(refused([&]{pmu::NativeCache(ram,linked/"filmstrips",[]{return true;}).validate();}),"linked final cache targets are rejected");
        fs::remove_all(ram/"filmstrips");fs::create_directory_symlink(cache,ram/"filmstrips");
        check(refused([&]{native.validate();}),"linked RAM cache is rejected");fs::remove(ram/"filmstrips");native.validate();
        save(cache/"temp_18e6325a58ab8462.img");int calls=0;
        check(refused([&]{pmu::NativeCache(ram,cache,[&]{return ++calls<2;}).clean();})&&fs::exists(cache/"temp_18e6325a58ab8462.img"),"MPC resuming before deletion aborts cleanup");
        fs::remove(cache/"temp_18e6325a58ab8462.img");
        if(mount((ram/"filmstrips").c_str(),cache.c_str(),nullptr,MS_BIND,nullptr))throw std::runtime_error("Native cache bind fixture failed");
        save(cache/"temp_18e6325a58ab8463.img");
        check(fs::exists(ram/"filmstrips/temp_18e6325a58ab8463.img"),"native writes reach the RAM source through a bind mount");
        check(native.clean()==1,"mounted native cache is cleaned without duplicate traversal");
        if(umount(cache.c_str()))throw std::runtime_error("Native cache fixture unmount failed");
        save(cache/"temp_18e6325a58ab8464.img");
        auto identity=[&](const fs::path& path){const int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)throw std::runtime_error("Cannot inspect source mount identity");const auto value=pmu::mountIdentity(fd);close(fd);return value;};
        const auto sourceMount=identity(disk);
        if(unshare(CLONE_NEWNS)||mount(nullptr,"/",nullptr,MS_PRIVATE|MS_REC,nullptr)||mount((ram/"filmstrips").c_str(),cache.c_str(),nullptr,MS_BIND,nullptr))throw std::runtime_error("Private service namespace fixture failed");
        check(!fs::exists(cache/"temp_18e6325a58ab8464.img"),"service namespace hides the previous persistent cache");
        check(identity(disk)!=sourceMount,"a private service namespace changes otherwise unchanged drive identities");
        pmu::NativeCache::serviceMountNamespace();
        check(fs::exists(cache/"temp_18e6325a58ab8464.img")&&native.prepare()==1,"preparation clears legacy cache before redirecting it to RAM");
        check(identity(disk)==sourceMount,"cache-only redirection preserves source mount identity");
        const auto cacheMount=identity(cache);check(native.prepare()==0&&identity(cache)==cacheMount,"repeated preparation reuses the RAM mount without stacking mounts");
        save(cache/"temp_18e6325a58ab8465.img");check(fs::exists(ram/"filmstrips/temp_18e6325a58ab8465.img"),"subsequent native cache writes reach RAM");
        check(native.prepare()==1,"stop-time preparation clears subsequent RAM images");
        if(umount(cache.c_str()))throw std::runtime_error("Prepared cache fixture unmount failed");
        const auto empty=disk/"empty";fs::create_directory(empty);pmu::NativeCache missing(ram,empty/"filmstrips",[]{return true;});
        missing.validate();check(missing.prepare()==0,"an absent native cache becomes an empty RAM mount");
        if(umount((empty/"filmstrips").c_str()))throw std::runtime_error("Empty cache fixture unmount failed");
        if(umount(ram.c_str()))throw std::runtime_error("RAM fixture unmount failed");
        mounted=false;fs::remove_all(root);
        std::cout<<checks<<" checks passed\n";return 0;
    }catch(const std::exception& error){if(mounted){umount(cache.c_str());umount(ram.c_str());}std::cerr<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
