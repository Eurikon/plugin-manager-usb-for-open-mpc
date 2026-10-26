#pragma once
#include "manager_update.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <map>
#include <algorithm>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace manager_update_fixture {
inline const std::vector<std::string> runtime={"manager","font.ttf","launch.sh","setup.sh","shortcut","shortcut-hook.so"};
inline void save(const fs::path& path,const std::string& data,mode_t mode=0600){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<data;out.close();if(!out||chmod(path.c_str(),mode))throw std::runtime_error("Cannot write update fixture");}
inline std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Missing update fixture");return {std::istreambuf_iterator<char>(in),{}};}
inline std::string binding(const Json& files){std::string value;for(const auto& name:runtime)value+=files.at("autorun/runtime/"+name).get<std::string>()+"  "+name+'\n';return value;}
inline Json inventory(const fs::path& root){Json files=Json::object();for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file()){const auto name=entry.path().lexically_relative(root).generic_string();if(name!="USB-SHA256.json")files[name]=sha256(entry.path());}return files;}
inline void writeHelperArchive(const fs::path& path,const std::map<std::string,std::string>& values){auto* ar=archive_write_new();archive_write_set_format_zip(ar);if(archive_write_open_filename(ar,path.c_str())!=ARCHIVE_OK){archive_write_free(ar);throw std::runtime_error("Cannot write helper fixture");}for(const auto& item:values){auto* entry=archive_entry_new();archive_entry_set_pathname(entry,("Devices/Plugin Manager helper/"+item.first).c_str());archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,item.second.size());if(archive_write_header(ar,entry)!=ARCHIVE_OK||archive_write_data(ar,item.second.data(),item.second.size())!=static_cast<la_ssize_t>(item.second.size())){archive_entry_free(entry);archive_write_free(ar);throw std::runtime_error("Cannot write helper member");}archive_entry_free(entry);}if(archive_write_close(ar)!=ARCHIVE_OK){archive_write_free(ar);throw std::runtime_error("Cannot finish helper fixture");}archive_write_free(ar);}
inline std::map<std::string,std::string> helper(const fs::path& path){std::map<std::string,std::string> values;auto* ar=archive_read_new();archive_read_support_format_zip(ar);if(archive_read_open_filename(ar,path.c_str(),65536)!=ARCHIVE_OK){archive_read_free(ar);throw std::runtime_error("Cannot read helper fixture");}archive_entry* entry=nullptr;while(archive_read_next_header(ar,&entry)==ARCHIVE_OK){const std::string full=archive_entry_pathname(entry);const std::string prefix="Devices/Plugin Manager helper/";const auto size=archive_entry_size(entry);if(full.compare(0,prefix.size(),prefix)||size<=0||size>1024*1024){archive_read_free(ar);throw std::runtime_error("Invalid helper fixture");}std::string data(static_cast<size_t>(size),'\0');size_t offset=0;while(offset<data.size()){const auto count=archive_read_data(ar,data.data()+offset,data.size()-offset);if(count<=0){archive_read_free(ar);throw std::runtime_error("Incomplete helper fixture");}offset+=static_cast<size_t>(count);}values[full.substr(prefix.size())]=std::move(data);}archive_read_free(ar);return values;}
inline Json bundle(const fs::path& root){for(const auto& name:runtime)save(root/"autorun/runtime"/name,"new "+name+'\n');for(const char* name:{"SDL2.txt","community-tools.txt","font-license.txt","json-license.txt","plugin-manager-usb.txt"})save(root/"autorun/runtime/licenses"/name,"license\n");save(root/".mpc-manager-id","mpc-usb-manager-v1\n");save(root/"autorun/open.sh","fixture open\n");save(root/"autorun/autorun.check",std::string(64,'a'));save(root/"autorun/autorun.json","{\"fixture\":true}\n");auto files=inventory(root);std::map<std::string,std::string> values;for(const char* name:{".owner","PluginManager.xpj","bootstrap.sh","controller.ini","manager-entry.so","plugin_manager.lua"})values[name]=std::string("fixture ")+name+'\n';values["manager-enabled"]="open-mpc-manager-reusable-v1\n";values["sd-install-capability"]="open-mpc-manager-sd-v1\n";values["runtime.sha256"]=binding(files);writeHelperArchive(root/"Plugin Manager helper.xpa",values);files=inventory(root);save(root/"USB-SHA256.json",files.dump(2)+'\n');return {{"bundle_root",root.string()},{"files",files},{"release",{{"latest_version","9.0.0"}}}};}
inline std::string escape(std::string value){size_t at=0;while((at=value.find(' ',at))!=std::string::npos){value.replace(at,1,"\\040");at+=4;}return value;}
inline bool under(const fs::path& root,const fs::path& path){const auto relative=path.lexically_relative(root);return !relative.empty()&&*relative.begin()!="..";}
}

struct ManagerUpdateFixture {
    fs::path source,session,root,base,card,sd;
    ManagerUpdatePaths paths;
    bool usbMounted=true,sdMounted=false;
    ManagerUpdateFixture(fs::path content,fs::path cache,const Json& staged):source(std::move(content)),session(std::move(cache)),root(session/"manager-update-fixture") {
        using namespace manager_update_fixture;
        paths.arch="armv7";
        paths.media=source.parent_path();paths.mountinfo=root/"mountinfo";paths.uuids=root/"uuids";paths.helper=root/"helper";paths.state=root/"state";paths.ram=root/"ram";paths.firmware=root/"MPC";paths.owner=geteuid();paths.protectedRoots={root/"internal"};base=paths.ram/"mpc-usb-session.ABC123";card=paths.media/"MPC SD";sd=card/"Plugin Manager USB";
        for(const auto& path:{source,session,paths.helper,paths.uuids,base,root/"internal"}){fs::create_directories(path);chmod(path.c_str(),0700);}
        const fs::path update=staged.at("bundle_root").get<std::string>();Json files=staged.at("files");
        for(const auto& item:files.items())save(source/item.key(),read(update/item.key()));
        save(source/"autorun/runtime/manager","previous manager runtime\n");save(source/"autorun/runtime/font.ttf","previous font\n");
        for(auto& item:files.items())item.value()=sha256(source/item.key());
        auto values=helper(update/"Plugin Manager helper.xpa");values["runtime.sha256"]=binding(files);writeHelperArchive(source/"Plugin Manager helper.xpa",values);files["Plugin Manager helper.xpa"]=sha256(source/"Plugin Manager helper.xpa");save(source/"USB-SHA256.json",files.dump(2)+'\n');
        for(const auto& item:values)if(item.first!="PluginManager.xpj")save(paths.helper/item.first,item.second);
        for(const auto& name:runtime)save(base/name,read(source/"autorun/runtime"/name),name=="manager"||name=="shortcut"?0700:0600);
        save(paths.firmware,"fixture firmware\n");paths.firmwareHash=sha256(paths.firmware);save(paths.state,base.string()+'\n'+source.string()+'\n');
        const auto usb=source,storage=card,internal=root/"internal",uuidRoot=paths.uuids;
        paths.fileDevice=[usb,storage,internal](const fs::path& path){if(under(storage,path))return makedev(253,2);if(under(usb,path))return makedev(253,1);if(under(internal,path))return makedev(253,0);struct stat st{};if(stat(path.c_str(),&st))throw std::runtime_error("Missing fixture device");return st.st_dev;};
        paths.blockDevice=[uuidRoot](const fs::path& path){if(path!=uuidRoot/"1234-ABCD"||read(path)!="253:2\n")throw std::runtime_error("Wrong fixture SD identity");return makedev(253,2);};paths.requireStopped=[]{};mounts();
    }
    void mounts(){using namespace manager_update_fixture;std::string text;if(usbMounted)text+="31 1 253:1 / "+escape(source.string())+" rw - exfat /dev/fixture-usb rw\n";if(sdMounted)text+="32 1 253:2 / "+escape(card.string())+" rw - exfat /dev/fixture-sd rw\n";save(paths.mountinfo,text);}
    void useArchitecture(const std::string& arch,Json& staged){
        using namespace manager_update_fixture;
        paths.arch=arch;
        const fs::path update=staged.at("bundle_root").get<std::string>();
        for(const char* name:{"manager","shortcut","shortcut-hook.so"}){
            const auto relative=fs::path("autorun/runtime")/managerRuntimePath(name,arch);
            save(update/relative,std::string("new native ")+name+'\n');
            save(source/relative,std::string(std::string(name)=="manager"?"old native ":"new native ")+name+'\n');
        }
        auto next=inventory(update),old=Json::parse(read(source/"USB-SHA256.json"));
        for(const char* name:{"manager","shortcut","shortcut-hook.so"}){
            const auto relative="autorun/runtime/"+managerRuntimePath(name,arch);
            old[relative]=sha256(source/relative);
        }
        auto bind=[&](const Json& files){std::string text;for(const auto& name:runtime){const auto relative=managerRuntimePath(name,arch);text+=files.at("autorun/runtime/"+relative).get<std::string>()+"  "+relative+'\n';}return text;};
        auto newHelper=helper(update/"Plugin Manager helper.xpa"),oldHelper=helper(source/"Plugin Manager helper.xpa");
        newHelper["manager-entry-aarch64.so"]=oldHelper["manager-entry-aarch64.so"]="fixture ARM64 helper\n";
        newHelper["runtime-aarch64.sha256"]=bind(next);oldHelper["runtime-aarch64.sha256"]=bind(old);
        writeHelperArchive(update/"Plugin Manager helper.xpa",newHelper);writeHelperArchive(source/"Plugin Manager helper.xpa",oldHelper);
        next=inventory(update);old["Plugin Manager helper.xpa"]=sha256(source/"Plugin Manager helper.xpa");staged["files"]=next;
        save(update/"USB-SHA256.json",next.dump(2)+'\n');save(source/"USB-SHA256.json",old.dump(2)+'\n');
        for(const auto& item:oldHelper)if(item.first!="PluginManager.xpj")save(paths.helper/item.first,item.second);
        for(const auto& name:runtime)save(base/name,read(source/"autorun/runtime"/managerRuntimePath(name,arch)),0600);
    }
    void installSd(bool mounted=true){using namespace manager_update_fixture;Json files=Json::object();const auto current=Json::parse(read(source/"USB-SHA256.json"));for(const auto& item:current.items())if(item.key()==".mpc-manager-id"||item.key().rfind("autorun/runtime/licenses/",0)==0||std::any_of(runtime.begin(),runtime.end(),[&](const std::string& name){return item.key()=="autorun/runtime/"+managerRuntimePath(name,paths.arch);})){save(sd/item.key(),read(source/item.key()));files[item.key()]=item.value();}save(sd/".manager-install.json",Json({{"schema",1},{"owner","plugin-manager-usb"},{"uuid","1234-ABCD"},{"installed",true},{"files",files}}).dump(2)+'\n');save(paths.helper/"installed-storage","open-mpc-manager-storage-v1\n1234-ABCD\n");if(mounted)save(paths.uuids/"1234-ABCD","253:2\n");sdMounted=mounted;mounts();}
    fs::path selectSd(){manager_update_fixture::save(paths.state,base.string()+'\n'+sd.string()+'\n');return sd;}
};
