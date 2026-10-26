#pragma once
#include "sd_install.hpp"
#include <fstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace sd_fixture {
inline void save(const fs::path& path,const std::string& value) {
    fs::create_directories(path.parent_path());std::ofstream file(path);file<<value;
    if(!file)throw std::runtime_error("Cannot create SD fixture");
}
inline std::string text(const fs::path& path) {
    std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read SD fixture");
    return {std::istreambuf_iterator<char>(file),{}};
}
inline bool under(const fs::path& base,const fs::path& path){const auto relative=path.lexically_relative(base);return !relative.empty()&&*relative.begin()!="..";}
inline std::string escaped(std::string value) {
    size_t at=0;while((at=value.find(' ',at))!=std::string::npos){value.replace(at,1,"\\040");at+=4;}return value;
}
struct Fixture {
    fs::path root,source,session,card,target;
    SdInstallPaths paths;
    Fixture(fs::path content,fs::path cache):root(cache/"sd-fixture"),source(std::move(content)),session(std::move(cache)) {
        fs::create_directories(source);fs::create_directories(session);
        paths.media=root/"media";paths.mountinfo=root/"mountinfo";paths.uuids=root/"by-uuid";
        paths.sys=root/"sys";paths.udev=root/"udev";
        paths.internal=root/"internal";paths.helper=root/"helper";paths.firmware=root/"MPC";paths.owner=geteuid();
        paths.protectedRoots={paths.internal};paths.arch="armv7";
        card=paths.media/"MPC SD";target=card/"Plugin Manager USB";
        for(const auto& directory:{paths.media,paths.uuids,paths.internal,paths.helper,card})fs::create_directories(directory);
        chmod(paths.helper.c_str(),0700);
        save(paths.firmware,"test MPC OS 3.10 binary\n");paths.firmwareHash=sha256(paths.firmware);
        save(paths.helper/"sd-install-capability","open-mpc-manager-sd-v1\n");
        save(source/".mpc-manager-id","mpc-usb-manager-v1\n");
        std::string manifest;
        for(const char* name:{"manager","font.ttf","launch.sh","setup.sh","shortcut","shortcut-hook.so"}){
            save(source/"autorun/runtime"/name,std::string("fixture runtime ")+name+'\n');manifest+=sha256(source/"autorun/runtime"/name)+"  "+name+'\n';
        }
        save(source/"autorun/runtime/licenses/manager.txt","Fixture license\n");
        save(paths.helper/"runtime.sha256",manifest);save(paths.uuids/"1234-ABCD","253:2\n");
        media("E:ID_BUS=usb\nE:ID_DRIVE_FLASH_SD=1\n");
        mount(true);
        const auto internal=paths.internal,media=paths.media,usb=source,uuidRoot=paths.uuids;
        paths.fileDevice=[internal,media,usb](const fs::path& path)->dev_t {
            if(under(internal,path)||path==internal)return makedev(253,0);
            if(under(usb,path)||path==usb)return makedev(253,1);
            if(under(media/"MPC SD",path)||path==media/"MPC SD")return makedev(253,2);
            if(under(media/"Other SD",path)||path==media/"Other SD")return makedev(253,3);
            struct stat st{};if(stat(path.c_str(),&st))throw std::runtime_error("Fixture device missing");return st.st_dev;
        };
        paths.blockDevice=[uuidRoot](const fs::path& path)->dev_t {
            if(path.parent_path()!=uuidRoot)throw std::runtime_error("Fixture identity outside UUID inventory");
            const auto value=text(path);unsigned majorValue,minorValue;char end;
            if(sscanf(value.c_str(),"%u:%u%c",&majorValue,&minorValue,&end)!=3||end!='\n')throw std::runtime_error("Invalid fixture device");
            return makedev(majorValue,minorValue);
        };
    }
    void mount(bool present,bool writable=true,unsigned id=42) {
        save(paths.mountinfo,present?std::to_string(id)+" 1 253:2 / "+escaped(card.string())+(writable?" rw,noexec":" ro,noexec")+" - exfat /dev/fixture-card rw\n":"");
    }
    void media(const std::string& properties,bool removable=true) {
        const auto disk=paths.sys/"devices/block/reader",partition=disk/"reader1",links=paths.sys/"dev/block";
        save(disk/"dev","253:16\n");save(disk/"removable",removable?"1\n":"0\n");
        save(partition/"dev","253:2\n");save(partition/"partition","1\n");
        save(paths.udev/"b253:16",properties);fs::create_directories(links);
        if(!fs::exists(links/"253:16"))fs::create_directory_symlink(disk,links/"253:16");
        if(!fs::exists(links/"253:2"))fs::create_directory_symlink(partition,links/"253:2");
    }
    void renameCard(const std::string& label) {
        const auto previous=paths.fileDevice;const auto renamed=paths.media/label;
        fs::rename(card,renamed);card=renamed;target=card/"Plugin Manager USB";mount(true);
        paths.fileDevice=[previous,renamed](const fs::path& path){return path==renamed||under(renamed,path)?makedev(253,2):previous(path);};
    }
    SdInstaller installer(){return SdInstaller(source,session,paths);}
};
}

inline SdInstaller makeSdInstallerFixture(fs::path source,fs::path session,const std::string& mode) {
    sd_fixture::Fixture fixture(std::move(source),std::move(session));
    if(mode=="sd-empty")fixture.mount(false);
    if(mode=="sd-unavailable")fs::remove(fixture.paths.helper/"sd-install-capability");
    auto installer=fixture.installer();
    if(mode=="sd-remove"||mode=="sd-remove-current"){
        installer.install("1234-ABCD");
        if(mode=="sd-remove-current")return SdInstaller(fixture.target,fixture.session,fixture.paths);
    }
    return installer;
}
