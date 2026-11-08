#include "plugin_transaction.hpp"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <dlfcn.h>

static bool rejectLinks=false;
extern "C" int symlink(const char* target,const char* link)noexcept{
    if(rejectLinks){errno=EOPNOTSUPP;return -1;}
    const auto original=reinterpret_cast<int(*)(const char*,const char*)>(dlsym(RTLD_NEXT,"symlink"));
    if(!original){errno=ENOSYS;return -1;}return original(target,link);
}

namespace fs=std::filesystem;
static int checks=0;
static void check(bool value,const char* name){if(!value)throw std::runtime_error(name);++checks;std::cout<<"PASS: "<<name<<'\n';}
static void save(const fs::path& file,const std::string& value){std::ofstream out(file);out<<value;out.close();if(!out)throw std::runtime_error("Cannot write fixture");}
static std::string read(const fs::path& file){std::ifstream in(file);if(!in)throw std::runtime_error("Cannot read fixture");return {std::istreambuf_iterator<char>(in),{}};}
template<class Function> static std::string failure(Function action){try{action();}catch(const std::exception& error){return error.what();}throw std::runtime_error("Expected rejection");}
static std::vector<fs::path> recoveries(const fs::path& root){std::vector<fs::path> result;for(const auto& entry:fs::directory_iterator(root))if(entry.path().filename().string().rfind(".pmu-plugin-recovery-",0)==0)result.push_back(entry.path());return result;}
int main(){
    char pattern[]="/tmp/pmu-plugin-transaction-XXXXXX";const char* name=mkdtemp(pattern);if(!name)return 1;const fs::path root=name;
    try{
        const auto payload=root/"payload",settings=root/"settings",history=root/"history",registry=root/"registry";
        fs::create_directory(payload);save(payload/"library.so","original");save(payload/"user-data","preserve");save(settings,"original settings");save(history,"original history");
        fs::permissions(payload/"library.so",fs::perms::owner_all);const auto permissions=fs::status(payload/"library.so").permissions();const auto modified=fs::last_write_time(payload/"library.so");
        fs::create_symlink("user-data",payload/"user-link");
        save(payload/"-user-data","preserve punctuation");
        if(geteuid()==0&&chown((payload/"user-data").c_str(),123,456))throw std::runtime_error("Cannot set fixture ownership");
        const std::vector<fs::path> paths{payload,settings,history,registry};
        {
            pmu::PluginTransaction transaction;transaction.prepare(paths,root,4096,true);
            const auto message=failure([&]{transaction.run([&]{fs::remove_all(payload);fs::create_directory(payload);save(payload/"library.so","changed");save(settings,"broken");fs::remove(history);fs::create_directory(registry);save(registry/"manifest","new");throw std::runtime_error("installer failed");});});
            check(message.find("restored and verified")!=std::string::npos,"failed operation reports verified recovery");
            check(read(payload/"library.so")=="original"&&read(payload/"user-data")=="preserve"&&read(payload/"-user-data")=="preserve punctuation"&&fs::read_symlink(payload/"user-link")=="user-data","recovery restores payload, user files and symlink");
            check(fs::status(payload/"library.so").permissions()==permissions&&fs::last_write_time(payload/"library.so")==modified,"recovery preserves file permissions and modification time");
            struct stat owner{};check(lstat((payload/"user-data").c_str(),&owner)==0&&(geteuid()!=0||(owner.st_uid==123&&owner.st_gid==456)),"recovery preserves user file ownership");
            check(read(settings)=="original settings"&&read(history)=="original history"&&!fs::exists(registry),"recovery restores settings, history and absent registry");
        }
        check(recoveries(root).empty(),"verified rollback removes recovery folders");
        {
            pmu::PluginTransaction transaction;transaction.prepare(paths,root,4096,false);transaction.run([&]{save(payload/"library.so","updated");save(settings,"updated settings");});
        }
        check(read(payload/"library.so")=="updated"&&read(settings)=="updated settings"&&recoveries(root).empty(),"successful operation commits and clears recovery folders");
        {
            pmu::PluginTransaction transaction;transaction.prepare(paths,root,0,false);save(settings,"changed concurrently");bool called=false;
            const auto message=failure([&]{transaction.run([&]{called=true;});});check(!called&&message.find("before installation")!=std::string::npos&&read(settings)=="changed concurrently","changed originals prevent script execution without overwriting concurrent data");
        }
        check(recoveries(root).empty(),"pre-operation rejection clears unused snapshots");
        {
            pmu::PluginTransaction transaction;transaction.prepare(paths,root,0,false);
            const auto message=failure([&]{transaction.run([&]{save(settings,"new settings");for(const auto& backup:recoveries(root))if(read(backup/"original-path")==payload.string()+"\n")save(backup/"saved/library.so","corrupted");throw std::runtime_error("injected failure");});});
            check(message.find("recovery incomplete")!=std::string::npos&&message.find("Keep recovery folders")!=std::string::npos,"damaged snapshot reports incomplete recovery and retained locations");
            check(read(settings)=="new settings"&&!recoveries(root).empty(),"all snapshots are checked before any rollback writes");
        }
        for(const auto& backup:recoveries(root))fs::remove_all(backup);
        {
            const auto parent=root/"replaceable";fs::create_directory(parent);save(parent/"file","old");pmu::PluginTransaction transaction;transaction.prepare({parent/"file"},root,0,false);
            const auto message=failure([&]{transaction.run([&]{fs::rename(parent,root/"detached");fs::create_directory(parent);save(parent/"file","replacement");throw std::runtime_error("drive replaced");});});
            check(message.find("recovery incomplete")!=std::string::npos&&read(parent/"file")=="replacement"&&!recoveries(root/"detached").empty(),"replaced parent prevents rollback writes to another drive or directory");
        }
        {
            const auto created=root/"new-target";pmu::PluginTransaction transaction;transaction.prepare({created},created,4096,true);
            failure([&]{transaction.run([&]{fs::create_directories(created/"plugin");save(created/"plugin/file","new");throw std::runtime_error("first install failed");});});
            check(!fs::exists(created),"first installation rollback removes newly created content target");
        }
        {
            const std::vector<fs::path> overlapping{payload,payload/"library.so"};pmu::PluginTransaction transaction;const auto message=failure([&]{transaction.prepare(overlapping,root,0,false);});check(message.find("Overlapping")!=std::string::npos,"overlapping protected paths are rejected");
        }
        {
            const auto linked=root/"linked-settings";fs::create_symlink(settings,linked);const std::vector<fs::path> protectedPaths{linked};pmu::PluginTransaction transaction;const auto message=failure([&]{transaction.prepare(protectedPaths,root,0,false);});check(message.find("linked target")!=std::string::npos,"linked protected target is rejected before mutation");
        }
        {
            const std::vector<fs::path> protectedPaths{settings};pmu::PluginTransaction transaction;rejectLinks=true;const auto message=failure([&]{transaction.prepare(protectedPaths,root,0,true);});rejectLinks=false;
            check(message.find("cannot create the symbolic links")!=std::string::npos&&recoveries(root).empty(),"filesystem without symbolic link support is rejected before snapshots or installer execution");
        }
        {
            const auto fifo=root/"fifo";if(mkfifo(fifo.c_str(),0600))throw std::runtime_error("Cannot create FIFO");const std::vector<fs::path> protectedPaths{fifo};pmu::PluginTransaction transaction;const auto message=failure([&]{transaction.prepare(protectedPaths,root,0,false);});check(message.find("Unsupported file")!=std::string::npos,"special files refuse snapshot preparation");
        }
        {
            const auto large=root/"large";{std::ofstream out(large);const std::string block(65536,'x');for(int i=0;i<256;++i)out<<block;}
            pmu::PluginTransaction transaction;transaction.prepare({large},root,0,false);failure([&]{transaction.run([&]{save(large,"truncated");throw std::runtime_error("large rollback");});});
            check(fs::file_size(large)==16*1024*1024&&read(large).front()=='x',"streamed recovery restores a large file");fs::remove(large);
        }
        if(const char* limited=std::getenv("PMU_TEST_SMALL_DRIVE")){
            const fs::path drive=limited;save(drive/"original","untouched");const std::vector<fs::path> protectedPaths{drive/"original"};pmu::PluginTransaction transaction;
            const auto message=failure([&]{transaction.prepare(protectedPaths,drive,0,false);});check(message.find("Not enough free space")!=std::string::npos&&read(drive/"original")=="untouched"&&recoveries(drive).empty(),"low-space rejection precedes snapshots and plugin changes");fs::remove(drive/"original");
        }else throw std::runtime_error("Set PMU_TEST_SMALL_DRIVE to a temporary filesystem smaller than 8 MiB");
        check(recoveries(root).empty(),"no unused recovery folders remain");fs::remove_all(root);std::cout<<checks<<" checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"; fixture retained: "<<root<<'\n';return 1;}
}
