#include "backend.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/sysmacros.h>
#include <unistd.h>

static int checks=0;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;std::cout<<"PASS: "<<message<<'\n';}
static void save(const fs::path& path,const std::string& value){std::ofstream file(path);file<<value;if(!file)throw std::runtime_error("Cannot write fixture");}
static std::string read(const fs::path& path){std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};}
template<class Function> static void rejects(Function action,const std::string& reason,const char* message){bool rejected=false;try{action();}catch(const std::exception& error){rejected=std::string(error.what()).find(reason)!=std::string::npos;}check(rejected,message);}
static std::string escape(std::string text){size_t at=0;while((at=text.find(' ',at))!=std::string::npos){text.replace(at,1,"\\040");at+=4;}return text;}
int main(){
    char pattern[]="/tmp/pmu-plugin-storage.XXXXXX";const auto created=mkdtemp(pattern);if(!created)return 1;const fs::path root=created;
    try{
        const auto ssd=root/"My SSD",other=root/"Other Drive",settings=root/"MPC.settings",mountinfo=root/"mountinfo",usb=root/"usb";
        for(const auto& path:{ssd,other,usb})fs::create_directory(path);
        PluginStoragePaths paths;paths.mounts=mountinfo;
        paths.device=[ssd,other](const fs::path& path){if(!fs::exists(path))throw std::runtime_error("Content drive is missing or disconnected");const auto text=path.string();if(text==ssd||text.rfind(ssd.string()+"/",0)==0)return makedev(8,1);if(text==other||text.rfind(other.string()+"/",0)==0)return makedev(8,2);return makedev(9,0);};
        auto mount=[&](std::string options="rw",int id=42,bool present=true){save(mountinfo,"1 0 9:0 / / rw - ext4 /dev/root rw\n"+(present?std::to_string(id)+" 1 8:1 / "+escape(ssd.string())+" "+options+" - exfat /dev/fixture-ssd rw\n":"")+"43 1 8:2 / "+escape(other.string())+" rw - ext4 /dev/fixture-other rw\n");};
        auto config=[&](std::string selected,std::string extra=""){return "<PROPERTIES><VALUE name=\"Content.Manager.Directory\" val=\""+selected+"\"/><VALUE name=\"Unrelated\" val=\"preserve exactly\"/>"+extra+"</PROPERTIES>\n";};
        mount();save(settings,config(ssd.string()));PluginStorage storage(settings,paths);
        const auto target=ssd/"Synths";const auto state=storage.automatic();
        check(state.at("path")==target.string()&&!state.at("exists").get<bool>()&&!state.at("registered").get<bool>(),"native preference resolves the actual mounted SSD and permits its missing Synths child");
        check(!fs::exists(target)&&read(settings)==config(ssd.string()),"automatic detection never creates folders or edits settings");
        fs::create_directory(other/"Synths");check(storage.automatic().at("path")==target.string(),"another existing Synths folder cannot override the firmware preference");
        fs::create_directory(ssd/"Subfolder");save(settings,config((ssd/"Subfolder").string()));check(storage.automatic().at("path")==target.string(),"saved content location is resolved by mounted filesystem identity");
        save(settings,config(ssd.string()));const auto original=read(settings);
        Manager manager(usb,root/"session","auto",settings,"aarch64",paths);
        check(manager.synths==target&&manager.pluginStorageInfo().at("automatic").get<bool>(),"manager uses firmware content preference in automatic mode");
        const Json item={{"origin","local"},{"path",(root/"package.zip").string()},{"sha",std::string(64,'0')},{"id","test"}};
        save(root/"package.zip","not an installable package");
        rejects([&]{manager.install(item);},"checksum mismatch","invalid package is rejected before destination preparation");
        check(!fs::exists(target)&&read(settings)==original,"rejected package leaves plugin folders and settings untouched");
        storage.prepare(target,state.at("identity"));
        check(fs::is_directory(target)&&read(settings).find("<Location>"+target.string()+"</Location>")!=std::string::npos,"confirmed preparation creates and registers the native Synths folder");
        check(read(settings).find("<VALUE name=\"Unrelated\" val=\"preserve exactly\"/>")!=std::string::npos&&read(settings).find("Content.Manager.Directory")!=std::string::npos,"registration preserves other settings and the MPC content-drive preference");
        bool backup=false;size_t backups=0;for(const auto& entry:fs::directory_iterator(root))if(entry.path().filename().string().find("MPC.settings.pmu-")==0){++backups;if(read(entry.path())==original)backup=true;}
        check(backup&&backups==1,"registration retains one exact original settings backup");const auto registered=read(settings);storage.prepare(target,state.at("identity"));
        check(read(settings)==registered,"repeated destination preparation does not duplicate registration");
        manager.selectPluginStorage(other/"Synths");check(manager.synths==other/"Synths"&&!manager.pluginStorageInfo().at("automatic").get<bool>(),"manual selection overrides automatic destination for the current session");
        check(read(settings)==registered,"selecting a location alone does not edit MPC settings");manager.selectPluginStorage({});check(manager.synths==target,"Automatic restores the firmware-selected drive");
        const auto keptSettings=read(settings);
        mount();save(settings,config(ssd.string(),"<VALUE name=\"defaultProgramPads\" val=\""+std::string(400000,'x')+"\"/><VALUE name=\"Other\" val=\"1\"/>"));
        check(storage.automatic().at("path")==target.string(),"a settings value hundreds of KB long (a Live III's defaultProgramPads is ~44 KB) neither crashes nor hides the content drive");
        save(settings,keptSettings);
        mount("rw,noexec");rejects([&]{storage.automatic();},"noexec","native noexec drive is rejected without substituting another drive");
        Manager blocked(usb,root/"blocked","auto",settings,"aarch64",paths);rejects([&]{blocked.install(item);},"noexec","unusable native destination blocks installation before download");
        check(blocked.synths.empty()&&read(settings)==registered,"failed automatic detection does not invent a destination or edit settings");
        blocked.selectPluginStorage(other/"Synths");blocked.validateTarget();check(blocked.synths==other/"Synths","manual selection remains usable when automatic destination is blocked");
        mount("ro");rejects([&]{storage.automatic();},"read-only","read-only content drive is rejected");
        mount("rw",44);rejects([&]{manager.validateTarget();},"changed","removal and reinsertion invalidate the previously selected mount identity");
        manager.selectPluginStorage({});manager.validateTarget();check(true,"explicit reselection accepts the current mount identity");
        mount("rw",44,false);rejects([&]{storage.automatic();},"mounted content drive","disconnected drive cannot resolve through its leftover mountpoint directory");
        mount();save(settings,"<PROPERTIES/>");rejects([&]{storage.automatic();},"not configured","missing content-drive preference has no guessed fallback");
        save(settings,config((root/"missing").string()));rejects([&]{storage.automatic();},"missing or disconnected","missing saved drive never falls back to another writable drive");
        save(settings,config(ssd.string(),"<VALUE name=\"Content.Manager.Directory\" val=\""+other.string()+"\"/>"));rejects([&]{storage.automatic();},"Duplicate","ambiguous native preferences are rejected");
        const auto custom=ssd/"Existing plugins";fs::create_directory(custom);
        save(settings,config(ssd.string(),"<VALUE name=\"SynthContentLocations\"><SynthContentLocations><Location>"+custom.string()+"/</Location></SynthContentLocations></VALUE><VALUE name=\"Recent\"><Location>"+(ssd/"unrelated").string()+"</Location></VALUE>"));
        check(storage.inspect(custom).at("registered").get<bool>(),"registered plugin folders are accepted with normalized trailing slash");
        rejects([&]{storage.inspect(ssd/"unrelated");},"Choose an existing folder","unrelated Location elements cannot authorize missing plugin destinations");
        const auto named=ssd/"My plugins";fs::create_directory(named);const auto namedState=storage.inspect(named);check(!namedState.at("registered").get<bool>(),"manually chosen folder need not be named Synths or previously registered");
        const auto listing=storage.folders(ssd);check(!listing.at("selectable").get<bool>()&&listing.at("folders").size()>=2,"manual folder browser lists content-drive folders and excludes drive root selection");
        const auto folderSettings=read(settings);storage.prepare(named,namedState.at("identity"));check(read(settings).find("<Location>"+named.string()+"</Location>")!=std::string::npos&&read(settings).find(custom.string())!=std::string::npos,"custom folder registration retains existing content locations");
        check(folderSettings!=read(settings),"custom registration occurs only during explicit preparation");
        fs::remove_all(target);fs::create_directory_symlink(other,target);rejects([&]{storage.automatic();},"symbolic link","symbolic-link destination is rejected");fs::remove(target);
        save(settings,config(other.string()));rejects([&]{manager.validateTarget();},"changed","changing MPC content-drive preference requires reselection");
        Manager legacy(usb,root/"legacy","/sdcard/Expansions",settings,"armv7",paths);check(legacy.synths=="/sdcard/Expansions"&&!legacy.pluginStorageInfo().at("available").get<bool>(),"Gen 1 retains its existing plugin destination independently of manager SD installation");
        fs::remove_all(root);std::cout<<checks<<" plugin storage checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<" (fixture "<<root<<")\n";fs::remove_all(root);return 1;}
}
