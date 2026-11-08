#include "settings_backups.hpp"
#include <iostream>

namespace fs=std::filesystem;
static int checks=0;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;std::cout<<"PASS: "<<message<<'\n';}
static void save(const fs::path& path,const std::string& text){std::ofstream output(path);output<<text;output.close();if(!output)throw std::runtime_error("Cannot save test input");}
static std::string read(const fs::path& path){std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};}
template<class Function> static bool refused(Function function){try{function();}catch(const std::exception&){return true;}return false;}
int main(){
    char pattern[]="/tmp/pmu-settings-backups-XXXXXX";const char* name=mkdtemp(pattern);if(!name)return 1;const fs::path root=name;
    try{
        const auto settings=root/"MPC.settings",store=root/"MPC.settings.pmu-backups",old=root/"MPC.settings.pmu-Old000",user=root/"user-file";
        save(settings,"settings unchanged\n");save(old,"old untracked backup\n");save(user,"user content\n");
        {pmu::SettingsBackups backups(settings);backups.finish();check(!fs::exists(store),"no backup folder is created when no new backup exists");}
        for(int i=0;i<13;++i){pmu::SettingsBackups backups(settings);const auto source=root/("MPC.settings.pmu-"+std::to_string(100000+i));save(source,"backup "+std::to_string(i));backups.finish();check(!fs::exists(source),"new backup is archived after collection");}
        auto entries=[&]{std::vector<fs::path> result;for(const auto& entry:fs::directory_iterator(store))if(entry.path().filename()!=".owner")result.push_back(entry.path());return result;};
        auto saved=entries();bool newest=false;for(const auto& path:saved)if(read(path)=="backup 12")newest=true;
        check(saved.size()==10&&newest,"retention keeps ten backups including the latest one");
        check(read(old)=="old untracked backup\n"&&read(settings)=="settings unchanged\n"&&read(user)=="user content\n","existing backups, MPC settings and unrelated user files remain unchanged");
        const auto modified=saved.front();save(modified,"user-modified backup\n");fs::last_write_time(modified,fs::file_time_type::clock::now()-std::chrono::hours(24*365));
        const auto unrelated=store/"personal-file";save(unrelated,"preserve\n");
        auto linkName=saved.back().filename().string();linkName.replace(linkName.size()-6,6,"Link00");const auto linked=store/linkName;fs::create_symlink(user,linked);
        auto hardName=saved.back().filename().string();hardName.replace(hardName.size()-6,6,"Hard00");const auto hard=store/hardName;fs::create_hard_link(user,hard);
        for(int i=0;i<12;++i){pmu::SettingsBackups backups(settings);const auto source=root/("MPC.settings.bak-library-20261108-"+std::to_string(100000+i));save(source,"package backup "+std::to_string(i));backups.finish();}
        check(read(modified)=="user-modified backup\n"&&read(unrelated)=="preserve\n","modified backups and unrecognized archive files survive retention");
        check(fs::is_symlink(linked)&&read(hard)=="user content\n"&&read(user)=="user content\n","symlink and hardlink entries cannot cause user-file deletion");
        {
            pmu::SettingsBackups backups(settings);const auto source=root/"MPC.settings.pmu-Lnk001";fs::create_symlink(user,source);backups.finish();check(fs::is_symlink(source)&&read(user)=="user content\n","linked new backup is ignored without following or moving it");
        }
        {
            const auto parent=root/"unowned";fs::create_directory(parent);save(parent/"MPC.settings","value");fs::create_directory(parent/"MPC.settings.pmu-backups");fs::permissions(parent/"MPC.settings.pmu-backups",fs::perms::owner_all);save(parent/"MPC.settings.pmu-backups/.owner","not this manager");
            pmu::SettingsBackups backups(parent/"MPC.settings");const auto source=parent/"MPC.settings.pmu-New001";save(source,"retain original");
            check(refused([&]{backups.finish();})&&read(source)=="retain original","unrecognized backup ownership refuses archiving and preserves the original file");
        }
        {
            const auto parent=root/"replaceable";fs::create_directory(parent);save(parent/"MPC.settings","value");pmu::SettingsBackups backups(parent/"MPC.settings");fs::rename(parent,root/"detached");fs::create_directory(parent);save(parent/"MPC.settings.pmu-New002","replacement");
            check(refused([&]{backups.finish();})&&read(parent/"MPC.settings.pmu-New002")=="replacement","parent replacement stops cleanup before another directory can be modified");
        }
        fs::remove_all(root);std::cout<<checks<<" settings backup checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"; fixture retained: "<<root<<'\n';return 1;}
}
