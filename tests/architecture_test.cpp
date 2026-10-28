#include "backend.hpp"
#include "display.hpp"
#include "startup/storage.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static void check(bool value,const char* name) {
    if(!value)throw std::runtime_error(name);
    std::cout<<"PASS: "<<name<<'\n';
}
static void save(const fs::path& path,const std::string& value) {
    std::ofstream output(path);output<<value;
    if(!output)throw std::runtime_error("Cannot write fixture");
}
int main() {
    char pattern[]="/tmp/manager-architecture.XXXXXX";
    const char* created=mkdtemp(pattern);
    if(!created)return 1;
    const fs::path root=created;
    const char* oldPath=getenv("PATH");const std::string savedPath=oldPath?oldPath:"";
    try {
        check(pmu_helper_directory_valid("/nvme/MPC Documents/Devices/Plugin Manager helper"),"Gen 2 internal helper location accepted");
        check(pmu_helper_directory_valid("/sdcard/MPC Documents/Devices/Plugin Manager helper"),"Gen 1 internal helper location accepted");
        for(const char* path:{"/media/USB/MPC Documents/Devices/Plugin Manager helper","/nvme/../MPC Documents/Devices/Plugin Manager helper","/nvme/MPC Documents/Devices/Other helper","nvme/MPC Documents/Devices/Plugin Manager helper"})
            check(!pmu_helper_directory_valid(path),"unapproved helper location refused");
        for(bool portrait:{false,true}) for(bool flipped:{false,true}) {
            const DisplayGeometry display{portrait,flipped};
            for(const auto& point:{std::make_pair(0,0),std::make_pair(1279,799),std::make_pair(640,400)}) {
                const auto physical=display.output(point.first,point.second);
                check(display.input(physical.first,physical.second)==point,"display touch mapping round trip");
            }
            check(display.width()*display.height()==1280*800,"display preserves full logical canvas");
            const auto edge=display.input(-100,-100);check(edge.first>=0&&edge.first<1280&&edge.second>=0&&edge.second<800,"out-of-bounds display input is clamped");
        }
        {
            std::string note;
            check(displayFlipFromSetting("180\n",note)&&note.empty(),"display-rotation 180 flips");
            check(!displayFlipFromSetting("0",note)&&note.empty(),"display-rotation 0 leaves the automatic choice");
            check(!displayFlipFromSetting("",note)&&note.empty(),"empty display-rotation leaves the automatic choice");
            check(!displayFlipFromSetting("90",note)&&!note.empty(),"unsupported display-rotation is ignored and named");
            const DisplayGeometry upright{true,false},flipped{true,true};
            check(flipped.input(upright.output(100,50).first,upright.output(100,50).second)==std::make_pair(1280-100,800-50),"a flipped panel turns touch by 180 degrees");
            check(devicetreePanelRotationHex("/nonexistent/panel-rotation").empty(),"missing panel-rotation property reads as empty");
        }
        fs::create_directories(root/"bin");fs::create_directories(root/"usb");fs::create_directories(root/"synths");
        const std::string digest(64,'a'),digest64(64,'b');
        auto row=[&](const std::string& id,bool legacy,bool modern){
            return "plugin\t"+id+"\t1.0.0\t1\tinstrument\t"+id+"\t"+id+"\tTest\t1\t"+
                (legacy?"1000\t"+digest+"\thttps://example.invalid/armv7.zip":"-\t-\t-")+"\t-\t1\t3.x\t2.17\t"+
                (modern?"2000\t"+digest64+"\thttps://example.invalid/aarch64.zip":"-\t-\t-")+"\n";
        };
        save(root/"catalog.tsv","#mpc-catalog-tsv 1\n"+row("both",true,true)+row("legacy",true,false)+row("modern",false,true));
        save(root/"catalog.json",Json{{"schema",1},{"plugins",Json::array({{{"id","both"},{"author","Fixture author"},{"versions",Json::array({{{"version","1.0.0"},{"sha256",digest},{"assets",{{"aarch64",{{"sha256",digest64}}}}}}})}}})}}.dump());
        save(root/"bin/wget","#!/bin/sh\nset -eu\ncase \"$5\" in */catalog.tsv) cp \"$MPC_ARCHITECTURE_FIXTURE/catalog.tsv\" \"$4\" ;; */catalog.json) cp \"$MPC_ARCHITECTURE_FIXTURE/catalog.json\" \"$4\" ;; *) exit 99 ;; esac\n");
        chmod((root/"bin/wget").c_str(),0700);
        setenv("PATH",((root/"bin").string()+":"+savedPath).c_str(),1);setenv("MPC_ARCHITECTURE_FIXTURE",root.c_str(),1);
        const auto library=root/"synths/module.so";save(library,"temporary installed-file marker");
        save(root/"settings.xml","<PROPERTIES><VALUE name=\"pluginList-arm\"><PLUGIN name=\"Legacy\" file=\""+library.string()+"\" uid=\"Leg1\"/></VALUE><VALUE name=\"pluginList-arm-64bit\"><PLUGIN name=\"Modern\" file=\""+library.string()+"\" uid=\"Mod1\"/></VALUE></PROPERTIES>");
        for(const std::string arch:{"armv7","aarch64"}) {
            Manager manager(root/"usb",root/arch,root/"synths",root/"settings.xml",arch);
            manager.openCatalog();check(manager.catalog.size()==3,"catalog preserves entries across device generations");
            const auto& both=manager.catalog[0];check(both.at("sha")== (arch=="aarch64"?digest64:digest),"catalog selects the device checksum");
            check(both.at("url")==std::string("https://example.invalid/")+arch+".zip","catalog selects the device URL");
            check(both.at("author")=="Fixture author","metadata follows the architecture-specific asset");
            check(manager.catalog[arch=="aarch64"?1:2].at("blocked_reason")!="","missing architecture is blocked before download");
            check(manager.catalog[arch=="aarch64"?2:1].at("blocked_reason")=="","available architecture remains installable");
            const auto installed=manager.installedPlugins();check(installed.size()==1&&installed[0].at("uid")== (arch=="aarch64"?"Mod1":"Leg1"),"installed inventory reads the correct registry");
            fs::create_directories(root/"synths/foreign");save(root/"synths/foreign/module.so","foreign architecture marker");
            save(root/"synths/foreign/mpc-plugin.json",Json{{"id","foreign"},{"name","Foreign"},{"version","1.0.0"},{"arch",arch=="aarch64"?"armv7":"aarch64"},{"skin","foreign"},{"so","module.so"}}.dump());
            check(manager.installedPlugins().size()==1,"installed metadata from the other generation is ignored");
            fs::remove_all(root/"synths/foreign");
            const auto package=root/arch/"package";fs::create_directory(package);
            save(package/"mpc-plugin.json",Json{{"schema",1},{"arch",arch=="aarch64"?"armv7":"aarch64"}}.dump());
            bool refused=false;try{manager.manifest(package);}catch(const std::exception& error){refused=std::string(error.what())=="Unsupported package manifest";}
            check(refused,"local package from the other generation is rejected");
        }
        setenv("PATH",savedPath.c_str(),1);unsetenv("MPC_ARCHITECTURE_FIXTURE");fs::remove_all(root);return 0;
    }catch(const std::exception& error){setenv("PATH",savedPath.c_str(),1);unsetenv("MPC_ARCHITECTURE_FIXTURE");fs::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
