#pragma once
#include "backend.hpp"
#include <fstream>
#include <sys/sysmacros.h>

inline PluginStoragePaths pluginStorageFixture(const fs::path& session,const fs::path& settings,bool blocked){
    const auto root=session/"plugin-storage",ssd=root/"MPC content drive",external=root/"External SSD";
    for(const auto& path:{root,ssd,external,external/"My plugins"})fs::create_directories(path);
    const auto mounts=root/"mountinfo";
    auto escaped=[](std::string text){size_t at=0;while((at=text.find(' ',at))!=std::string::npos){text.replace(at,1,"\\040");at+=4;}return text;};
    {std::ofstream file(mounts);file<<"1 0 9:0 / / rw - ext4 /dev/root rw\n42 1 8:1 / "<<escaped(ssd.string())<<(blocked?" rw,noexec":" rw")<<" - exfat /dev/fixture-ssd rw\n43 1 8:2 / "<<escaped(external.string())<<" rw - ext4 /dev/fixture-external rw\n";if(!file)throw std::runtime_error("Cannot create plugin storage fixture");}
    {std::ofstream file(settings);file<<"<PROPERTIES><VALUE name=\"Content.Manager.Directory\" val=\""<<ssd.string()<<"\"/></PROPERTIES>";if(!file)throw std::runtime_error("Cannot create plugin settings fixture");}
    PluginStoragePaths paths;paths.mounts=mounts;
    paths.device=[ssd,external](const fs::path& path){if(!fs::exists(path))throw std::runtime_error("Fixture drive is missing");if(path==ssd||path.string().rfind(ssd.string()+"/",0)==0)return makedev(8,1);if(path==external||path.string().rfind(external.string()+"/",0)==0)return makedev(8,2);return makedev(9,0);};return paths;
}
