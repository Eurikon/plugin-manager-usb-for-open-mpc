#include "sd_install_fixture.hpp"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks=0;
void check(bool condition,const char* name){if(!condition)throw std::runtime_error(name);++checks;std::cout<<"PASS "<<name<<'\n';}
template<class Function> void rejects(Function function,const char* name){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}check(rejected,name);}
struct Test {
    fs::path root;
    sd_fixture::Fixture data;
    SdInstaller installer;
    explicit Test(const fs::path& base):root(base),data(base/"usb",base/"session"),installer(data.installer()){}
    ~Test(){std::error_code error;fs::remove_all(root,error);}
};
}
int main(){
    fs::path base;
    try{
        auto temporary=(fs::canonical(fs::temp_directory_path())/"manager-sd-tests.XXXXXX").string();std::vector<char> path(temporary.begin(),temporary.end());path.push_back(0);char* made=mkdtemp(path.data());if(!made)throw std::runtime_error("Cannot create test root");base=made;
        {
            Test t(base/"normal");check(t.installer.available(),"updated helper enables SD option");
            check(t.installer.targets().size()==1&&t.installer.targets()[0]["uuid"]=="1234-ABCD","mounted noexec SD selected by stable filesystem UUID");
            check(!t.installer.inspect()["configured"].get<bool>(),"new helper has no SD installation");
            sd_fixture::save(t.data.source/"manager-preferences.json","{\"schema\":1,\"dark_theme\":true}\n");
            sd_fixture::save(t.data.source/"plugin-history.json","{\"schema\":1,\"plugins\":{}}\n");
            float progress=0;t.installer.install("1234-ABCD",{},[&](float value){progress=value;});
            check(progress==1&&t.installer.inspect()["installed"].get<bool>(),"verified installation activates selected card");
            check(t.data.target==t.data.card/"Plugin Manager USB"&&!fs::exists(t.data.card/"OPEN MPC"),"manager installs directly at card root without an extra parent folder");
            check(sd_fixture::text(t.data.paths.helper/"installed-storage")=="open-mpc-manager-storage-v1\n1234-ABCD\n","helper activation uses exact agreed configuration");
            check(sd_fixture::text(t.data.target/"manager-preferences.json")==sd_fixture::text(t.data.source/"manager-preferences.json"),"first installation copies saved preferences");
            sd_fixture::save(t.data.target/"local-plugins/user.zip","user package");sd_fixture::save(t.data.target/"user.txt","unrelated file");sd_fixture::save(t.data.target/"manager-preferences.json","{\"user\":true}\n");
            t.installer.install("1234-ABCD");
            check(sd_fixture::text(t.data.target/"manager-preferences.json")=="{\"user\":true}\n"&&fs::exists(t.data.target/"local-plugins/user.zip"),"reinstallation preserves SD preferences and local packages");
            SdInstaller current(t.data.target,t.data.session,t.data.paths);check(current.inspect()["current"].get<bool>(),"installed runtime identifies current source");
            current.remove();
            check(!fs::exists(t.data.target/".mpc-manager-id")&&!fs::exists(t.data.target/"autorun/runtime")&&!fs::exists(t.data.paths.helper/"installed-storage"),"removal disables registration and removes only owned runtime");
            check(fs::exists(t.data.target/"manager-preferences.json")&&fs::exists(t.data.target/"plugin-history.json")&&fs::exists(t.data.target/"local-plugins/user.zip")&&fs::exists(t.data.target/"user.txt"),"removal preserves settings history local packages and unrelated files");
            t.installer.install("1234-ABCD");check(t.installer.inspect()["installed"].get<bool>(),"removed SD installation can be reinstalled");
        }
        {
            Test t(base/"aarch64");t.data.paths.arch="aarch64";
            std::string manifest;
            for(const char* name:{"manager","font.ttf","launch.sh","setup.sh","shortcut","shortcut-hook.so"}){
                const auto relative=managerRuntimePath(name,"aarch64");
                if(relative!=name)sd_fixture::save(t.data.source/"autorun/runtime"/relative,std::string("ARM64 runtime ")+name+'\n');
                manifest+=sha256(t.data.source/"autorun/runtime"/relative)+"  "+relative+'\n';
            }
            sd_fixture::save(t.data.paths.helper/"runtime-aarch64.sha256",manifest);
            SdInstaller installer(t.data.source,t.data.session,t.data.paths);
            installer.install("1234-ABCD");
            check(installer.inspect()["installed"]==true,"ARM64 SD installation is detected using its selected runtime");
            check(fs::is_regular_file(t.data.target/"autorun/runtime/aarch64/manager")&&!fs::exists(t.data.target/"autorun/runtime/manager"),"ARM64 SD copy contains the native executable without the Gen1 executable");
            const auto owner=Json::parse(sd_fixture::text(t.data.target/".manager-install.json"));
            check(owner["files"]["autorun/runtime/aarch64/manager"]==sha256(t.data.source/"autorun/runtime/aarch64/manager"),"ARM64 ownership inventory records the selected executable checksum");
            sd_fixture::save(t.data.target/"local-plugins/user.zip","user package");
            installer.remove();
            check(!fs::exists(t.data.target/"autorun/runtime")&&fs::exists(t.data.target/"local-plugins/user.zip"),"ARM64 removal cleans native runtime while preserving user packages");
        }
        {Test t(base/"capability");fs::remove(t.data.paths.helper/"sd-install-capability");check(!t.installer.available(),"missing helper capability hides SD feature");rejects([&]{t.installer.install("1234-ABCD");},"missing capability prevents writes");}
        {Test t(base/"firmware");sd_fixture::save(t.data.paths.firmware,"different firmware");rejects([&]{t.installer.install("1234-ABCD");},"different firmware cannot install manager to SD");check(!fs::exists(t.data.target),"firmware refusal does not create SD folder");}
        {Test t(base/"helper-link");fs::rename(t.data.paths.helper/"sd-install-capability",t.data.paths.helper/"real-capability");fs::create_symlink("real-capability",t.data.paths.helper/"sd-install-capability");check(!t.installer.available(),"linked capability cannot enable installer");}
        {Test t(base/"readonly");t.data.mount(true,false);check(t.installer.targets().empty(),"read-only cards are not offered");rejects([&]{t.installer.install("1234-ABCD");},"read-only selected card cannot be written");}
        {Test t(base/"ssd-label");t.data.media("E:ID_BUS=ata\nE:ID_MODEL=Samsung_SSD_870_QVO_1TB\n",false);check(t.installer.targets().empty(),"SSD named MPC SD is not offered");rejects([&]{t.installer.install("1234-ABCD");},"direct UUID cannot install to SSD");check(fs::is_empty(t.data.card)&&!fs::exists(t.data.paths.helper/"installed-storage"),"rejected SSD receives no installation files or activation");}
        {Test t(base/"usb-flash");t.data.media("E:ID_BUS=usb\nE:ID_DRIVE_FLASH=1\n");check(t.installer.targets().empty(),"removable USB flash drive is not offered as SD");rejects([&]{t.installer.install("1234-ABCD");},"direct UUID cannot install to non-SD USB flash");check(fs::is_empty(t.data.card),"rejected USB flash remains unchanged");}
        {Test t(base/"unknown-media");fs::remove(t.data.paths.udev/"b253:16");check(t.installer.targets().empty(),"missing SD media identification fails closed");rejects([&]{t.installer.install("1234-ABCD");},"unknown physical media cannot be selected directly");check(fs::is_empty(t.data.card),"unknown media remains unchanged");}
        {Test t(base/"renamed-card");t.data.renameCard("Travel Samples");auto installer=t.data.installer();check(installer.targets().size()==1&&installer.targets()[0]["label"]=="Travel Samples","renamed physical SD remains selectable");installer.install("1234-ABCD");check(installer.inspect()["installed"].get<bool>()&&fs::exists(t.data.target/"autorun/runtime/manager"),"renamed SD installs by UUID without a label requirement");}
        {Test t(base/"nonremovable-sd");t.data.media("E:ID_DRIVE_FLASH_SD=1\n",false);check(t.installer.targets().empty(),"SD flag cannot admit nonremovable storage");rejects([&]{t.installer.install("1234-ABCD");},"nonremovable storage is rejected by direct UUID");}
        {Test t(base/"partition-only-flag");t.data.media("E:ID_BUS=usb\n");sd_fixture::save(t.data.paths.udev/"b253:2","E:ID_DRIVE_FLASH_SD=1\n");check(t.installer.targets().empty(),"partition flag cannot replace physical parent SD identification");}
        {Test t(base/"conflicting-sd-flags");t.data.media("E:ID_DRIVE_FLASH_SD=1\nE:ID_DRIVE_FLASH_SD=0\n");check(t.installer.targets().empty(),"conflicting media classification is rejected");}
        {Test t(base/"wrong-sysfs-device");sd_fixture::save(t.data.paths.sys/"devices/block/reader/reader1/dev","253:3\n");check(t.installer.targets().empty(),"sysfs partition identity must match the mounted block device");}
        {Test t(base/"missing-sysfs-device");fs::remove(t.data.paths.sys/"dev/block/253:2");check(t.installer.targets().empty(),"missing kernel media identity fails closed");}
        {Test t(base/"legacy-ssd-removal");t.installer.install("1234-ABCD");sd_fixture::save(t.data.target/"user.txt","preserve");t.data.media("E:ID_BUS=ata\n",false);check(t.installer.targets().empty()&&t.installer.inspect()["can_remove"].get<bool>(),"owned legacy SSD installation remains removable but is not offered");rejects([&]{t.installer.install("1234-ABCD");},"owned legacy SSD cannot be updated");t.installer.remove();check(!fs::exists(t.data.target/"autorun/runtime")&&!fs::exists(t.data.paths.helper/"installed-storage")&&sd_fixture::text(t.data.target/"user.txt")=="preserve","legacy SSD removal preserves user files and disables activation");}
        {Test t(base/"legacy-unknown-removal");t.installer.install("1234-ABCD");fs::remove(t.data.paths.udev/"b253:16");check(t.installer.inspect()["can_remove"].get<bool>(),"owned installation remains removable when media classification disappears");t.installer.remove();check(!fs::exists(t.data.paths.helper/"installed-storage"),"unknown legacy media can be safely unregistered");}
        {Test t(base/"space");t.data.paths.availableBytes=[](const fs::path&){return uint64_t(0);};auto installer=t.data.installer();rejects([&]{installer.install("1234-ABCD");},"insufficient card space is refused before staging");check(!fs::exists(t.data.target),"full card remains unchanged");}
        {Test t(base/"absent");t.data.mount(false);check(t.installer.targets().empty(),"missing card produces an empty selection");rejects([&]{t.installer.install("1234-ABCD");},"missing selected card cannot be written");}
        {Test t(base/"duplicate");sd_fixture::save(t.data.paths.mountinfo,sd_fixture::text(t.data.paths.mountinfo)+sd_fixture::text(t.data.paths.mountinfo));rejects([&]{t.installer.targets();},"duplicate mount identity is refused");}
        {Test t(base/"duplicate-uuid");sd_fixture::save(t.data.paths.uuids/"5678-ABCD","253:2\n");rejects([&]{t.installer.targets();},"multiple UUID aliases are refused");}
        {Test t(base/"uuid-contract");rejects([&]{t.installer.install("not-a-filesystem-UUID");},"nonhex filesystem identity is refused");rejects([&]{t.installer.install(std::string(65,'a'));},"filesystem identity longer than helper limit is refused");}
        {Test t(base/"internal");t.data.paths.fileDevice=[](const fs::path&){return makedev(253,2);};auto installer=t.data.installer();check(installer.targets().empty(),"internal storage is never offered even with positive SD media identification");rejects([&]{installer.install("1234-ABCD");},"direct UUID cannot select protected internal storage");}
        {Test t(base/"protected-data");const auto data=t.data.root/"data",mount=t.data.paths.media/"az01-internal";fs::create_directories(data);fs::create_directories(mount);t.data.paths.protectedRoots.push_back(data);const auto original=t.data.paths.fileDevice;t.data.paths.fileDevice=[original,data,mount](const fs::path& path){return path==data||path==mount?makedev(253,4):original(path);};sd_fixture::save(t.data.paths.uuids/"9988-AABB","253:4\n");sd_fixture::save(t.data.paths.mountinfo,sd_fixture::text(t.data.paths.mountinfo)+"43 1 253:4 / "+sd_fixture::escaped(mount.string())+" rw - ext4 /dev/system-data rw\n");auto installer=t.data.installer();check(installer.targets().size()==1&&installer.targets()[0]["uuid"]=="1234-ABCD","system data partition is excluded even with a valid filesystem UUID");rejects([&]{installer.install("9988-AABB");},"system data partition cannot be selected directly");}
        {Test t(base/"unowned");sd_fixture::save(t.data.target/"user.txt","preserve");rejects([&]{t.installer.install("1234-ABCD");},"unowned destination collision is refused");check(sd_fixture::text(t.data.target/"user.txt")=="preserve","unowned destination remains intact");}
        {Test t(base/"source-hash");sd_fixture::save(t.data.source/"autorun/runtime/manager","changed");rejects([&]{t.installer.install("1234-ABCD");},"changed source fails trusted runtime checksum");check(!fs::exists(t.data.target),"bad source is not installed");}
        {Test t(base/"source-link");fs::remove(t.data.source/"autorun/runtime/font.ttf");fs::create_symlink("manager",t.data.source/"autorun/runtime/font.ttf");rejects([&]{t.installer.install("1234-ABCD");},"linked runtime source is refused");}
        {Test t(base/"operation-lock");bool nested=false;t.installer.install("1234-ABCD",[&](const std::string&){if(!nested){nested=true;rejects([&]{t.installer.install("1234-ABCD");},"overlapping SD operation is refused");}});check(nested&&t.installer.inspect()["installed"].get<bool>(),"first SD operation survives a rejected overlapping request");}
        {Test t(base/"target-link");fs::create_directories(t.data.card/"other");fs::create_directory_symlink("other",t.data.target);rejects([&]{t.installer.install("1234-ABCD");},"linked SD destination is refused");}
        {Test t(base/"removed-during-copy");rejects([&]{t.installer.install("1234-ABCD",{},[&](float value){if(value>0)t.data.mount(false);});},"card removal during copy prevents activation");check(!fs::exists(t.data.paths.helper/"installed-storage"),"removed card never becomes startup target");}
        {Test t(base/"changed-mount");rejects([&]{t.installer.install("1234-ABCD",{},[&](float value){if(value>0)t.data.mount(true,true,43);});},"reinsertion with a changed mount ID prevents activation");}
        {Test t(base/"media-changed-during-copy");rejects([&]{t.installer.install("1234-ABCD",{},[&](float value){if(value>0)t.data.media("E:ID_BUS=usb\n");});},"SD classification is rechecked during copying");check(!fs::exists(t.data.target)&&!fs::exists(t.data.paths.helper/"installed-storage"),"lost SD identification prevents activation");}
        {Test t(base/"copy-failure");rejects([&]{t.installer.install("1234-ABCD",{},[](float value){if(value>0)throw std::runtime_error("injected write failure");});},"copy failure does not activate installation");check(!fs::exists(t.data.target)&&!fs::exists(t.data.paths.helper/"installed-storage"),"copy failure leaves no installed destination");}
        {Test t(base/"activation-failure");sd_fixture::save(t.data.paths.helper/".installed-storage.tmp","existing");rejects([&]{t.installer.install("1234-ABCD");},"activation collision is refused");check(!fs::exists(t.data.target)&&!fs::exists(t.data.paths.helper/"installed-storage"),"activation failure rolls back newly installed runtime");}
        {Test t(base/"update-failure");t.installer.install("1234-ABCD");const auto before=sha256(t.data.target/"autorun/runtime/manager");sd_fixture::save(t.data.paths.helper/".installed-storage.tmp","existing");rejects([&]{t.installer.install("1234-ABCD");},"update activation failure is reported");check(sha256(t.data.target/"autorun/runtime/manager")==before&&t.installer.inspect()["installed"].get<bool>(),"failed update restores prior runtime and registration");}
        {Test t(base/"unknown-runtime");t.installer.install("1234-ABCD");sd_fixture::save(t.data.target/"autorun/runtime/user.txt","preserve");rejects([&]{t.installer.install("1234-ABCD");},"unowned runtime file prevents destructive update");t.installer.remove();check(sd_fixture::text(t.data.target/"autorun/runtime/user.txt")=="preserve","removal preserves unowned runtime file");}
        {Test t(base/"modified-runtime");t.installer.install("1234-ABCD");sd_fixture::save(t.data.target/"autorun/runtime/manager","user edit");rejects([&]{t.installer.install("1234-ABCD");},"modified runtime prevents overwrite");t.installer.remove();check(sd_fixture::text(t.data.target/"autorun/runtime/manager")=="user edit"&&!fs::exists(t.data.target/".mpc-manager-id"),"removal preserves modified runtime while disabling launch");}
        {Test t(base/"missing-runtime");t.installer.install("1234-ABCD");fs::remove_all(t.data.target/"autorun/runtime");check(!t.installer.inspect()["installed"].get<bool>()&&t.installer.inspect()["can_remove"].get<bool>(),"incomplete runtime remains removable");t.installer.remove();check(!fs::exists(t.data.paths.helper/"installed-storage"),"incomplete installation can be unregistered");}
        {Test t(base/"interrupted-remove");t.installer.install("1234-ABCD");rejects([&]{t.installer.remove({},[](float value){if(value>0)throw std::runtime_error("interrupted removal");});},"interrupted removal is reported");check(!fs::exists(t.data.paths.helper/"installed-storage")&&fs::exists(t.data.paths.helper/"installed-storage.removing"),"interrupted removal leaves launch disabled and recovery identity saved");const auto state=t.installer.inspect();check(state["removing"].get<bool>()&&state["can_remove"].get<bool>()&&!state["configured"].get<bool>(),"interrupted removal remains discoverable and resumable");rejects([&]{t.installer.install("1234-ABCD");},"installation waits for unfinished removal");t.installer.remove();t.installer.remove();check(!fs::exists(t.data.paths.helper/"installed-storage.removing")&&!fs::exists(t.data.target/".mpc-manager-id"),"repeated removal finishes and clears recovery record");}
        {Test t(base/"malformed-config");sd_fixture::save(t.data.paths.helper/"installed-storage","open-mpc-manager-storage-v1\n../../bad\n");chmod((t.data.paths.helper/"installed-storage").c_str(),0600);rejects([&]{t.installer.inspect();},"malformed saved identity is refused");}
        {Test t(base/"marker-last");t.installer.install("1234-ABCD");bool checked=false;t.installer.remove([&](const std::string& value){if(value.find("SD manager removed.")==0){checked=true;check(fs::exists(t.data.target/".mpc-manager-id")&&!fs::exists(t.data.paths.helper/"installed-storage")&&!fs::exists(t.data.target/"autorun/runtime"),"marker remains until configuration and code removal finish");}});check(checked&&!fs::exists(t.data.target/".mpc-manager-id"),"marker is removed only after cleanup");}
        fs::remove_all(base);std::cout<<checks<<" SD storage checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';if(!base.empty()){std::error_code ignored;fs::remove_all(base,ignored);}return 1;}
}
