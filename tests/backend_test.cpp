#include "backend.hpp"
#include "version.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <regex>
#include <archive.h>
#include <archive_entry.h>
#include <map>
#include <cerrno>
#include <sys/wait.h>
static void check(bool ok,const char* name){if(!ok)throw std::runtime_error(name);std::cout<<"PASS: "<<name<<'\n';}
static std::string text(const fs::path& path){std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read test file");return {std::istreambuf_iterator<char>(file),{}};}
static void save(const fs::path& path,const std::string& value){std::ofstream file(path);file<<value;if(!file)throw std::runtime_error("Cannot write test file");}
static size_t checksumCalls(const fs::path& path){const auto value=fs::exists(path)?text(path):std::string();return std::count(value.begin(),value.end(),'\n');}
static std::string xmlEscape(const std::string& value){std::string result;for(const char c:value){switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;case '"':result+="&quot;";break;case '\'':result+="&apos;";break;default:result+=c;}}return result;}
static void managerReleaseChecks(Manager& manager) {
    const auto response=manager.session/"manager-response.json",status=manager.session/"manager-status",calls=manager.session/"manager-requests";
    const auto wget=fs::path("/tmp/manager-test-bin/wget");const auto originalWget=text(wget),settings=text(manager.settings);
    const auto catalog=manager.catalog;const auto catalogError=manager.catalogError;
    auto usbState=[&](){std::vector<std::pair<std::string,std::string>> state;for(const auto& entry:fs::recursive_directory_iterator(manager.usb))if(entry.is_regular_file())state.emplace_back(entry.path().string(),sha256(entry.path()));std::sort(state.begin(),state.end());return state;};
    const auto originalUsb=usbState();
    const std::string current=MPC_MANAGER_VERSION;std::smatch match;
    check(std::regex_match(current,match,std::regex("([0-9]+)\\.([0-9]+)\\.([0-9]+)")),"manager version has one canonical numeric source");
    std::vector<unsigned long> parts{std::stoul(match[1].str()),std::stoul(match[2].str()),std::stoul(match[3].str())};
    auto version=[](const std::vector<unsigned long>& value){return std::to_string(value[0])+"."+std::to_string(value[1])+"."+std::to_string(value[2]);};
    auto newer=parts;newer[2]+=10;auto older=parts;size_t lower=older.size();while(lower&&older[lower-1]==0)--lower;check(lower!=0,"manager release fixture has an older numeric version");--older[lower-1];
    auto metadata=[](const std::string& value){return Json{{"draft",false},{"prerelease",false},{"tag_name","v"+value},{"html_url","https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v"+value}};};
    const auto release=metadata(current);
    save(status,"0");save(wget,"#!/bin/sh\nset -eu\nprintf '%s\\n' request >>/tmp/manager-session/manager-requests\n[ \"$#\" -eq 8 ] && [ \"$1\" = -T ] && [ \"$2\" = 15 ] && [ \"$3\" = -t ] && [ \"$4\" = 1 ] && [ \"$5\" = '--header=Accept: application/vnd.github+json' ] && [ \"$6\" = -O ] && [ \"$7\" = /tmp/manager-session/manager-release.json ] && [ \"$8\" = 'https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest' ] || exit 9\ncp /tmp/manager-session/manager-response.json \"$7\"\nexit \"$(cat /tmp/manager-session/manager-status)\"\n");
    auto verify=[&](const Json& value,bool available,const char* label){
        save(response,value.dump());const auto before=checksumCalls(calls);const auto result=manager.checkManagerUpdate();
        check(result.at("current_version")==current&&result.at("latest_version")==value.at("tag_name").get<std::string>().substr(1)&&result.at("update_available")==available&&result.at("development")==bool(MPC_MANAGER_DEVELOPMENT)&&result.at("release_url")==value.at("html_url")&&result.at("message").is_string()&&!result.at("message").get<std::string>().empty(),label);
        check(checksumCalls(calls)==before+1&&!fs::exists(manager.session/"manager-release.json"),"manager check makes one bounded metadata request and removes response");
        if(MPC_MANAGER_DEVELOPMENT)check(result.at("message").get<std::string>().find("Development build")!=std::string::npos&&result.at("message").get<std::string>().find("up to date")==std::string::npos,"development manager never reports itself up to date");
        return result;
    };
    auto reject=[&](const Json& value,const char* label){save(response,value.dump());bool rejected=false;try{manager.checkManagerUpdate();}catch(const std::exception&){rejected=true;}check(rejected&&!fs::exists(manager.session/"manager-release.json"),label);};
    try {
        const auto equal=verify(release,false,"equal published manager version is not an update");
        if(!MPC_MANAGER_DEVELOPMENT)check(equal.at("message")=="Plugin Manager is up to date.","clean release build reports equal latest release");
        verify(metadata(version(older)),false,"older published manager release never offers a downgrade");
        verify(metadata(version(newer)),true,"manager patch versions compare numerically rather than lexically");
        newer=parts;++newer[1];newer[2]=0;verify(metadata(version(newer)),true,"manager minor version takes precedence over patch");
        newer=parts;++newer[0];newer[1]=newer[2]=0;verify(metadata(version(newer)),true,"manager major version takes precedence over minor");
        auto changed=release;changed["draft"]=true;reject(changed,"draft manager release is rejected");
        changed=release;changed["prerelease"]=true;reject(changed,"manager prerelease is rejected");
        for(const char* field:{"draft","prerelease","tag_name","html_url"}){changed=release;changed.erase(field);reject(changed,"manager release requires all stable identity fields");changed=release;changed[field]=0;reject(changed,"manager release rejects incorrectly typed identity fields");}
        for(const char* tag:{"1.0.4","v1.0.4-rc1","v1.0.4+build","v01.0.4","v1.0","v1.0.4.1","v1.0.999999999999999999999","v1.0.4;touch /tmp/manager-remote-executed"}){changed=release;changed["tag_name"]=tag;reject(changed,"manager release accepts only bounded stable numeric tags");}
        for(const char* url:{"https://github.com/other/repository/releases/tag/v1.0.3","https://untrusted.invalid/release","http://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v1.0.3","https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v1.0.3?run=script"}){changed=release;changed["html_url"]=url;reject(changed,"manager release URL stays pinned to its own repository and exact tag");}
        reject(Json::array(),"non-object manager metadata is rejected");
        reject(Json{{"message","API rate limit exceeded"}},"manager reports GitHub API rate limiting");
        save(response,"{broken");bool rejected=false;try{manager.checkManagerUpdate();}catch(const std::exception&){rejected=true;}check(rejected&&!fs::exists(manager.session/"manager-release.json"),"malformed manager metadata is rejected and cleaned");
        save(response,std::string(1024*1024+1,' '));rejected=false;try{manager.checkManagerUpdate();}catch(const std::exception&){rejected=true;}check(rejected&&!fs::exists(manager.session/"manager-release.json"),"oversized manager metadata is rejected and cleaned");
        save(status,"4");reject(release,"offline manager check fails without retaining partial metadata");
        save(status,"8");reject(release,"HTTP or rate-limited manager check fails without retaining partial metadata");save(status,"0");
        changed=release;changed["assets"]=Json::array({{{"browser_download_url","https://untrusted.invalid/update.sh;touch /tmp/manager-remote-executed"}}});changed["body"]="$(touch /tmp/manager-remote-executed)";
        verify(changed,false,"manager check ignores release assets and executable release text");
        check(!fs::exists("/tmp/manager-remote-executed")&&manager.catalog==catalog&&manager.catalogError==catalogError&&text(manager.settings)==settings&&usbState()==originalUsb&&!fs::exists(manager.session/"download.zip"),"manager update lookup never executes remote content or changes catalogs, plugin ZIPs or settings");
    } catch(...) {save(wget,originalWget);throw;}
    save(wget,originalWget);for(const auto& file:{response,status,calls})fs::remove(file);
}
struct ManagerZipEntry {std::string name,data;mode_t type=AE_IFREG;};
static void managerZip(const fs::path& target,const std::vector<ManagerZipEntry>& entries) {
    auto* output=archive_write_new();if(!output)throw std::runtime_error("Cannot allocate ZIP fixture");
    try {
        if(archive_write_set_format_zip(output)!=ARCHIVE_OK||archive_write_open_filename(output,target.c_str())!=ARCHIVE_OK)throw std::runtime_error("Cannot open ZIP fixture");
        for(const auto& file:entries){auto* entry=archive_entry_new();if(!entry)throw std::runtime_error("Cannot allocate ZIP fixture entry");archive_entry_set_pathname(entry,file.name.c_str());archive_entry_set_filetype(entry,file.type);archive_entry_set_perm(entry,0644);archive_entry_set_size(entry,file.type==AE_IFREG?file.data.size():0);if(file.type==AE_IFLNK)archive_entry_set_symlink(entry,file.data.c_str());
            const auto status=archive_write_header(output,entry);archive_entry_free(entry);if(status!=ARCHIVE_OK)throw std::runtime_error("Cannot create ZIP fixture entry");if(file.type==AE_IFREG&&archive_write_data(output,file.data.data(),file.data.size())!=static_cast<la_ssize_t>(file.data.size()))throw std::runtime_error("Cannot write ZIP fixture entry");}
        if(archive_write_close(output)!=ARCHIVE_OK)throw std::runtime_error("Cannot close ZIP fixture");
        archive_write_free(output);
    }catch(...){archive_write_free(output);throw;}
}
static void managerStageChecks(Manager& manager) {
    const auto source=manager.session/"manager-source.zip",response=manager.session/"manager-response.json",status=manager.session/"manager-download-status",calls=manager.session/"manager-stage-requests",hashInput=manager.session/"manager-hash-input";
    const auto wget=fs::path("/tmp/manager-test-bin/wget");const auto originalWget=text(wget),settings=text(manager.settings);const auto originalCatalog=manager.catalog;
    const std::string current=MPC_MANAGER_VERSION;const auto dot=current.rfind('.');const auto latest=current.substr(0,dot+1)+std::to_string(std::stoul(current.substr(dot+1))+1);
    const auto assetName="plugin-manager-usb-v"+latest+".zip",assetUrl="https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/download/v"+latest+"/"+assetName;
    std::string elf(52,'\0');elf[0]=0x7f;elf[1]='E';elf[2]='L';elf[3]='F';elf[4]=elf[5]=elf[6]=1;elf[16]=3;elf[18]=40;elf[40]=52;
    std::map<std::string,std::string> bundle={{".mpc-manager-id","mpc-usb-manager-v1\n"},{"autorun/autorun.check",std::string(64,'a')},{"autorun/open.sh","#!/bin/sh\ntouch /tmp/manager-remote-executed\n"}};
    for(const char* name:{"manager","shortcut","shortcut-hook.so"})bundle[std::string("autorun/runtime/")+name]=elf;
    for(const char* name:{"font.ttf","launch.sh","setup.sh"})bundle[std::string("autorun/runtime/")+name]="fixture runtime\n";
    for(const char* name:{"plugin-manager-usb.txt","SDL2.txt","community-tools.txt","font-license.txt","json-license.txt"})bundle[std::string("autorun/runtime/licenses/")+name]="fixture license\n";
    Json autorunFiles=Json::array();for(const auto& file:bundle)if(file.first.rfind("autorun/runtime/",0)==0)autorunFiles.push_back(file.first.substr(8));
    bundle["autorun/autorun.json"]=Json{{"value0",{{"files",autorunFiles},{"scripts",Json::array({{{"path","open.sh"}}})}}}}.dump();
    auto entries=[&](const std::map<std::string,std::string>& content){Json manifest=Json::object();std::vector<ManagerZipEntry> result;for(const auto& file:content){save(hashInput,file.second);manifest[file.first]=sha256(hashInput);result.push_back({file.first,file.second});}result.push_back({"USB-SHA256.json",manifest.dump()});result.push_back({"local-plugins/","",AE_IFDIR});return result;};
    const auto originalEntries=entries(bundle);
    auto metadata=[&](){return Json{{"draft",false},{"prerelease",false},{"tag_name","v"+latest},{"html_url","https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v"+latest},{"assets",Json::array({{{"name",assetName},{"state","uploaded"},{"browser_download_url",assetUrl},{"size",fs::file_size(source)},{"digest","sha256:"+sha256(source)}}})}};};
    save(status,"0");save(wget,"#!/bin/sh\nset -eu\nprintf '%s\\n' request >>/tmp/manager-session/manager-stage-requests\nif [ \"$#\" -eq 8 ] && [ \"$8\" = 'https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest' ]; then cp /tmp/manager-session/manager-response.json \"$7\"; exit; fi\n[ \"$#\" -eq 7 ] && [ \"$1\" = -T ] && [ \"$2\" = 15 ] && [ \"$3\" = -t ] && [ \"$4\" = 1 ] && [ \"$5\" = -O ] && [ \"$7\" = '"+assetUrl+"' ] || exit 9\ncase \"$6\" in /tmp/manager-session/manager-update.*/manager.zip) ;; *) exit 9;; esac\ncp /tmp/manager-session/manager-source.zip \"$6\"\nexit \"$(cat /tmp/manager-session/manager-download-status)\"\n");
    auto clean=[&](){for(const auto& entry:fs::directory_iterator(manager.session))if(entry.path().filename().string().rfind("manager-update.",0)==0)return false;return true;};
    auto checked=[&](const std::vector<ManagerZipEntry>& files){managerZip(source,files);save(response,metadata().dump());return manager.checkManagerUpdate();};
    auto reject=[&](const Json& release,const char* label){bool rejected=false;try{manager.stageManagerUpdate(release);}catch(const std::exception&){rejected=true;}check(rejected&&clean(),label);};
    try {
        bool interrupted=false;try{runProcess({"sleep","30"},[](){throw std::runtime_error("test progress interruption");});}catch(const std::exception& error){interrupted=std::string(error.what())=="test progress interruption";}
        int childStatus=0;errno=0;const auto child=waitpid(-1,&childStatus,WNOHANG);check(interrupted&&child==-1&&errno==ECHILD,"throwing process progress callback terminates and reaps its child");
        auto release=checked(originalEntries);check(release.at("downloadable")==true&&release.at("asset").at("url")==assetUrl,"manager checks expose only a verified own-repository download");
        const auto before=checksumCalls(calls);std::vector<float> amounts;const auto staged=manager.stageManagerUpdate(release,{},[&](float value){amounts.push_back(value);});const fs::path root=staged.at("bundle_root").get<std::string>();
        check(checksumCalls(calls)==before+2&&staged.at("release")==release&&fs::exists(root/"autorun/runtime/manager")&&staged.at("files").size()==bundle.size()&&text(root/".mpc-manager-id")==bundle.at(".mpc-manager-id")&&amounts.front()==0&&amounts.back()==1,"manager stage refetches metadata, downloads once and returns verified runtime files");
        check(!fs::exists(root.parent_path()/"manager.zip")&&!fs::exists("/tmp/manager-remote-executed"),"manager stage removes ZIP and never executes its launch scripts");fs::remove_all(root.parent_path());
        interrupted=false;try{manager.stageManagerUpdate(release,{},[](float){throw std::runtime_error("test staging interruption");});}catch(const std::exception& error){interrupted=std::string(error.what())=="test staging interruption";}check(interrupted&&clean(),"throwing staging progress callback removes its temporary directory");
        auto universal=bundle;universal["Plugin Manager helper.xpa"]="fixture 3.10 helper\n";release=checked(entries(universal));const auto universalStage=manager.stageManagerUpdate(release);const fs::path universalRoot=universalStage.at("bundle_root").get<std::string>();check(fs::is_regular_file(universalRoot/"Plugin Manager helper.xpa"),"manager staging accepts the universal helper asset");fs::remove_all(universalRoot.parent_path());
        auto changed=metadata();changed["assets"][0].erase("digest");save(response,changed.dump());auto noAsset=manager.checkManagerUpdate();check(!noAsset.at("downloadable").get<bool>(),"manager download requires a GitHub SHA-256 digest");reject(noAsset,"missing manager checksum prevents downloading");
        changed=metadata();changed["assets"][0]["browser_download_url"]="https://untrusted.invalid/manager.zip";save(response,changed.dump());check(!manager.checkManagerUpdate().at("downloadable").get<bool>(),"manager asset download URL must match repository, tag and filename");
        changed=metadata();changed["assets"].push_back(changed["assets"][0]);save(response,changed.dump());check(!manager.checkManagerUpdate().at("downloadable").get<bool>(),"ambiguous manager ZIP assets disable downloading");
        changed=metadata();changed["assets"][0]["size"]=64ULL*1024*1024+1;save(response,changed.dump());check(!manager.checkManagerUpdate().at("downloadable").get<bool>(),"oversized manager ZIP metadata disables downloading");
        release=checked(originalEntries);changed=metadata();changed["assets"][0]["digest"]="sha256:"+std::string(64,'0');save(response,changed.dump());const auto guarded=checksumCalls(calls);reject(release,"changed manager asset is rejected before downloading");check(checksumCalls(calls)==guarded+1,"changed release causes only a fresh metadata request");
        release=manager.checkManagerUpdate();reject(release,"manager ZIP content must match GitHub digest");
        release=checked(originalEntries);save(status,"4");reject(release,"failed manager download removes partial stage");save(status,"0");
        auto invalid=originalEntries;invalid.push_back({"../manager-escape","escape"});reject(checked(invalid),"manager ZIP traversal is rejected and staging removed");
        invalid=originalEntries;invalid.push_back({"/tmp/manager-escape","escape"});reject(checked(invalid),"manager ZIP absolute path is rejected");
        invalid=originalEntries;invalid.push_back(originalEntries.front());reject(checked(invalid),"duplicate manager ZIP paths are rejected");
        invalid=originalEntries;invalid.push_back({"autorun/runtime/linked","../../outside",AE_IFLNK});reject(checked(invalid),"manager ZIP symlinks are rejected");
        invalid=originalEntries;invalid.push_back({"local-plugins/plugin.zip","unwanted plugin"});reject(checked(invalid),"manager ZIP cannot carry plugin packages");
        invalid=originalEntries;invalid[0].data="modified without inventory change";reject(checked(invalid),"manager internal inventory checks every file digest");
        auto badBundle=bundle;badBundle[".mpc-manager-id"]="different manager\n";reject(checked(entries(badBundle)),"manager marker identity is checked after hashes");
        badBundle=bundle;badBundle.erase("autorun/runtime/manager");reject(checked(entries(badBundle)),"manager bundle requires complete runtime");
        badBundle=bundle;badBundle["autorun/runtime/manager"]="not an ARM program";reject(checked(entries(badBundle)),"manager staged executable must be ARM ELF");
        badBundle=bundle;badBundle["autorun/autorun.check"]="invalid";reject(checked(entries(badBundle)),"manager autorun checksum format is verified");
        badBundle=bundle;badBundle["autorun/autorun.json"]="{}";reject(checked(entries(badBundle)),"manager autorun metadata is verified");
        badBundle=bundle;auto autorun=Json::parse(badBundle.at("autorun/autorun.json"));autorun["value0"]["files"].erase(0);badBundle["autorun/autorun.json"]=autorun.dump();reject(checked(entries(badBundle)),"manager autorun inventory must cover all runtime files");
        invalid=originalEntries;for(auto& file:invalid)if(file.name=="USB-SHA256.json")file.data="{}";reject(checked(invalid),"manager ZIP inventory must cover all regular files");
        release=checked(originalEntries);changed=metadata();changed["tag_name"]="v"+current;changed["html_url"]="https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v"+current;save(response,changed.dump());reject(release,"manager stage will not replace a current build with an equal release");
        check(clean()&&!fs::exists("/tmp/manager-escape")&&!fs::exists("/tmp/manager-remote-executed")&&manager.catalog==originalCatalog&&text(manager.settings)==settings&&!fs::exists(manager.session/"download.zip"),"manager stage failures preserve plugin state and cannot escape staging or execute packages");
    }catch(...){save(wget,originalWget);throw;}
    save(wget,originalWget);for(const auto& file:{source,response,status,calls,hashInput})fs::remove(file);
}
// A repository that publishes other things too (so "latest" is the wrong release): the release is found by tag prefix
// plus the catalog version, and the package by its exact name.
static void taggedReleaseChecks(Manager& manager) {
    const auto catalog=manager.catalog;auto item=catalog.at(0);
    const std::string repo="test-owner/mixed-releases",tagPrefix="plugin-g2-v",assetPrefix="Plugin-G2-";
    const auto version=item.at("version").get<std::string>();const auto tag=tagPrefix+version;const auto name=assetPrefix+version+"-mpc-armv7.zip";
    const auto endpoint="https://api.github.com/repos/"+repo+"/releases/tags/"+tag;
    auto urlFor=[&](const std::string& releaseTag,const std::string& file){return "https://github.com/"+repo+"/releases/download/"+releaseTag+"/"+file;};
    auto packageUrl=[&](const std::string& file){return urlFor(tag,file);};
    item["url"]=packageUrl(name);manager.catalog={item};
    const auto response=manager.session/"tagged-response.json",calls=manager.session/"tagged-requests";
    const auto wget=fs::path("/tmp/manager-test-bin/wget");const auto originalWget=text(wget);
    save(wget,"#!/bin/sh\nset -eu\nprintf '%s\\n' \"$8\" >>/tmp/manager-session/tagged-requests\n[ \"$#\" -eq 8 ] && [ \"$1\" = -T ] && [ \"$2\" = 15 ] && [ \"$3\" = -t ] && [ \"$4\" = 1 ] && [ \"$5\" = '--header=Accept: application/vnd.github+json' ] && [ \"$6\" = -O ] && [ \"$8\" = '"+endpoint+"' ] || exit 9\ncp /tmp/manager-session/tagged-response.json \"$7\"\n");
    auto releaseFor=[&](const std::string& releaseTag,const std::string& file){return Json{{"draft",false},{"prerelease",false},{"tag_name",releaseTag},{"assets",Json::array({{{"name",file},{"state","uploaded"},{"size",std::stoull(item.at("size").get<std::string>())},{"browser_download_url",urlFor(releaseTag,file)},{"digest","sha256:"+item.at("sha").get<std::string>()}}})}};};
    auto verify=[&](const Json& metadata,bool accepted,const char* label){save(response,metadata.dump());bool passed=true;try{manager.verifyLatestPlugin(item.at("id").get<std::string>(),ReleaseSource(repo,tagPrefix,assetPrefix));}catch(const std::exception&){passed=false;}check(passed==accepted&&!fs::exists(manager.session/"latest-release.json"),label);};
    try {
        verify(releaseFor(tag,name),true,"tagged release and exact package name match the catalog (the tag endpoint is the only request the fake wget allows)");
        check(text(calls).find("/releases/latest")==std::string::npos&&text(calls).find("/releases/tags/"+tag)!=std::string::npos,"a prefixed release is never looked up through releases/latest");
        const auto bogusTag=tagPrefix+"999.0.0";manager.catalog[0]["url"]=urlFor(bogusTag,name); // every other field consistent with the bogus tag, so only the tag check can reject
        verify(releaseFor(bogusTag,name),false,"a release whose tag is not prefix plus the catalog version is rejected even when its package URL matches the catalog");
        manager.catalog[0]["url"]=packageUrl(name);
        const auto other="Other-"+version+"-mpc-armv7.zip";manager.catalog[0]["url"]=packageUrl(other);
        verify(releaseFor(tag,other),false,"a package that is not named prefix plus version plus -mpc-<arch>.zip is rejected even when everything else matches");
        manager.catalog[0]["url"]=packageUrl(name);
        auto draft=releaseFor(tag,name);draft["prerelease"]=true;verify(draft,false,"a pre-release is not offered");
    } catch(...) {manager.catalog=catalog;save(wget,originalWget);throw;}
    manager.catalog=catalog;save(wget,originalWget);for(const auto& file:{response,calls})fs::remove(file);
}
static void latestReleaseChecks(Manager& manager,const Json& primary,const Json& olderVersion,const fs::path& library) {
    const auto catalog=manager.catalog;auto item=catalog.at(0);
    auto repo=primary.value("source_repo","");if(!std::regex_match(repo,std::regex("[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9][A-Za-z0-9_.-]*")))repo="test-owner/test-repository";
    const auto endpoint="https://api.github.com/repos/"+repo+"/releases/latest";
    const auto tag="v"+item.at("version").get<std::string>();const auto name=item.at("id").get<std::string>()+"-"+item.at("version").get<std::string>()+"-mpc-armv7.zip";
    item["url"]="https://github.com/"+repo+"/releases/download/"+tag+"/"+name;manager.catalog={item};
    const auto response=manager.session/"latest-response.json",status=manager.session/"latest-status",calls=manager.session/"latest-requests";
    const auto wget=fs::path("/tmp/manager-test-bin/wget");const auto originalWget=text(wget),settings=text(manager.settings);
    const Json release={{"draft",false},{"prerelease",false},{"tag_name",tag},{"assets",Json::array({{{"name",name},{"state","uploaded"},{"size",std::stoull(item.at("size").get<std::string>())},{"browser_download_url",item.at("url")},{"digest","sha256:"+item.at("sha").get<std::string>()}}})}};
    save(status,"0");save(wget,"#!/bin/sh\nset -eu\nprintf '%s\\n' request >>/tmp/manager-session/latest-requests\nif [ \"$#\" -eq 5 ] && [ \"$5\" = '"+item.at("url").get<std::string>()+"' ]; then cp /tmp/manager-fixtures/package.zip \"$4\"; exit; fi\n[ \"$#\" -eq 8 ] && [ \"$1\" = -T ] && [ \"$2\" = 15 ] && [ \"$3\" = -t ] && [ \"$4\" = 1 ] && [ \"$5\" = '--header=Accept: application/vnd.github+json' ] && [ \"$6\" = -O ] && [ \"$8\" = '"+endpoint+"' ] || exit 9\n[ \"$(cat /tmp/manager-session/latest-status)\" = 0 ] || exit 8\ncp /tmp/manager-session/latest-response.json \"$7\"\nif [ -f /tmp/manager-session/latest-guard-copy ]; then while IFS= read -r source && IFS= read -r target; do mkdir -p \"$(dirname \"$target\")\"; cp \"$source\" \"$target\"; done </tmp/manager-session/latest-guard-copy; fi\n");
    auto verify=[&](const Json& metadata,bool accepted,const char* label){save(response,metadata.dump());bool passed=true;try{manager.verifyLatestPlugin(item.at("id").get<std::string>(),repo);}catch(const std::exception&){passed=false;}check(passed==accepted&&!fs::exists(manager.session/"latest-release.json"),label);};
    try {
        verify(release,true,"latest stable package matches supplied catalog metadata without downloading it");
        const auto before=checksumCalls(calls);verify(release,true,"latest release is rechecked on every explicit selection");check(checksumCalls(calls)==before+1,"latest selection makes one bounded metadata request");
        auto changed=release;changed["tag_name"]=item.at("version");changed["assets"][0]["browser_download_url"]="https://github.com/"+repo+"/releases/download/"+item.at("version").get<std::string>()+"/"+name;manager.catalog[0]["url"]=changed["assets"][0]["browser_download_url"];verify(changed,true,"release tags accept versions with or without v prefix");manager.catalog={item};
        changed=release;changed["tag_name"]="v999999.0.0";verify(changed,false,"newer upstream release blocks stale catalog offer");
        changed=release;changed["assets"][0]["digest"]="sha256:"+std::string(64,'0');verify(changed,false,"latest digest mismatch blocks offer");
        changed=release;changed["assets"][0].erase("digest");verify(changed,false,"latest release without checksum evidence is rejected");
        changed=release;changed["assets"][0]["size"]=1;verify(changed,false,"latest package size mismatch is rejected");
        changed=release;changed["assets"][0]["browser_download_url"]="https://untrusted.invalid/package.zip";verify(changed,false,"latest package download remains pinned to repository release");
        changed=release;changed["assets"].push_back(changed["assets"][0]);verify(changed,false,"ambiguous latest ARMv7 assets are rejected");
        changed=release;changed["assets"]=Json::array();verify(changed,false,"latest release without ARMv7 package is rejected");
        changed=release;changed["assets"][0]["state"]="new";verify(changed,false,"unfinished release upload is rejected");
        changed=release;changed["draft"]=true;verify(changed,false,"draft release is rejected");
        changed=release;changed["prerelease"]=true;verify(changed,false,"prerelease is rejected");
        verify(Json{{"message","API rate limit exceeded"}},false,"GitHub API error is rejected");
        save(response,"{broken");bool rejected=false;try{manager.verifyLatestPlugin(item.at("id").get<std::string>(),repo);}catch(const std::exception&){rejected=true;}check(rejected&&!fs::exists(manager.session/"latest-release.json"),"malformed latest response is rejected and removed");
        save(status,"8");verify(release,false,"failed metadata request is rejected");save(status,"0");
        const auto requests=checksumCalls(calls);
        auto rejectBeforeRequest=[&](const char* id,const std::string& repository,const char* label){bool invalid=false;try{manager.verifyLatestPlugin(id,repository);}catch(const std::exception&){invalid=true;}check(invalid&&checksumCalls(calls)==requests,label);};
        rejectBeforeRequest(item.at("id").get_ref<const std::string&>().c_str(),"../repository","invalid repository rejected before request");
        rejectBeforeRequest("",repo,"missing catalog entry rejected before request");
        manager.catalog.push_back(item);rejectBeforeRequest(item.at("id").get_ref<const std::string&>().c_str(),repo,"duplicate catalog entry rejected before request");manager.catalog={item};
        manager.catalog[0]["os_compat"]=Json::array({"2.x"});rejectBeforeRequest(item.at("id").get_ref<const std::string&>().c_str(),repo,"incompatible catalog offer rejected before request");manager.catalog={item};
        manager.catalog[0]["url"]="https://untrusted.invalid/package.zip";rejectBeforeRequest(item.at("id").get_ref<const std::string&>().c_str(),repo,"untrusted catalog repository rejected before request");manager.catalog={item};
        check(text(manager.settings)==settings&&!fs::exists(manager.session/"download.zip")&&!fs::exists(manager.usb/"plugin-history.json")&&fs::is_empty(manager.synths),"latest lookup never downloads package or changes plugin installation");
        const auto folder=manager.synths/primary.at("skin").get<std::string>();
        const auto secondary=manager.synths/(primary.at("skin").get<std::string>()+" secondary");
        auto installed=[&](Json metadata,const fs::path& destination){fs::create_directories(destination);metadata["skin"]=destination.filename().string();fs::copy_file(library,destination/metadata.at("so").get<std::string>(),fs::copy_options::overwrite_existing);save(destination/"mpc-plugin.json",metadata.dump());};
        auto checkOffer=[&](const std::string& label,bool allowed,const char* message){const auto offer=manager.installationOffer(item);check(offer.at("label")==label&&offer.at("allowed")==allowed,message);};
        checkOffer("Install",true,"absent plugin offers installation");
        installed(primary,folder);checkOffer("Installed",false,"equal installed release disables welcome installation");
        const auto guardedRequests=checksumCalls(calls);bool blocked=false;try{manager.installLatestPlugin(item.at("id").get<std::string>(),repo);}catch(const std::exception&){blocked=true;}check(blocked&&checksumCalls(calls)==guardedRequests,"installed guard rejects before any remote request");
        auto metadata=primary;metadata["version"]="v"+primary.at("version").get<std::string>();installed(metadata,folder);checkOffer("Installed",false,"installed versions normalize v prefix");
        metadata["version"]="999999.0.0";installed(metadata,folder);checkOffer("Installed",false,"newer installed release prevents downgrade");
        metadata["version"]="unknown";installed(metadata,folder);checkOffer("Installed",false,"unparseable installed release is not reinstalled");
        metadata["version"]="";installed(metadata,folder);checkOffer("Installed",false,"missing installed release is not reinstalled");
        metadata=primary;metadata["id"]="existing-"+primary.at("id").get<std::string>();metadata["uid"]="different";installed(metadata,folder);checkOffer("Installed",false,"matching skin blocks duplicate identity");fs::remove_all(folder);
        metadata=primary;metadata["id"]="existing-"+primary.at("id").get<std::string>();installed(metadata,secondary);checkOffer("Installed",false,"matching UID blocks duplicate copy under another skin");fs::remove_all(secondary);
        metadata=primary;metadata["uid"]="different";installed(metadata,secondary);checkOffer("Installed",false,"matching ID blocks duplicate copy with different skin and UID");fs::remove_all(secondary);
        auto incompatible=item;incompatible["os_compat"]=Json::array({"2.x"});check(manager.installationOffer(incompatible).at("label")=="Unavailable"&&!manager.installationOffer(incompatible).at("allowed").get<bool>(),"absent incompatible package is unavailable");
        auto registration=[&](const fs::path& file){save(manager.settings,"<PROPERTIES><VALUE name=\"SynthContentLocations\"><SynthContentLocations><Location>/sdcard/Expansions</Location></SynthContentLocations></VALUE><VALUE name=\"pluginList-arm\"><KNOWNPLUGINS><PLUGIN file=\""+xmlEscape(file.string())+"\" uid=\""+xmlEscape(primary.at("uid").get<std::string>())+"\" version=\"0.0.0\"/></KNOWNPLUGINS></VALUE></PROPERTIES>");};
        fs::create_directories(folder);fs::copy_file(library,folder/primary.at("so").get<std::string>());registration(folder/primary.at("so").get<std::string>());checkOffer("Installed",false,"MPC SDK version is not treated as an installed release version");fs::remove_all(folder);save(manager.settings,settings);
        const auto external=manager.session/"guard-external"/primary.at("so").get<std::string>();fs::create_directories(external.parent_path());fs::copy_file(library,external);registration(external);checkOffer("Installed",false,"external installation prevents a second SD copy");save(manager.settings,settings);fs::remove_all(external.parent_path());
        if(olderVersion.is_string()) {
            metadata=primary;metadata["version"]=olderVersion;installed(metadata,folder);checkOffer("Update",true,"strictly older managed release offers update");
            installed(primary,secondary);checkOffer("Installed",false,"any equal duplicate blocks update even when first match is older");
            installed(metadata,secondary);checkOffer("Update",true,"all matching managed releases must be older to allow update");fs::remove_all(secondary);fs::remove_all(folder);
        }else std::cout<<"SKIP: older installed release checks require a supplied version with a numeric predecessor\n";
        save(response,release.dump());const auto snapshot=manager.session/"guard-manifest.json";save(snapshot,primary.dump());
        const auto inject=manager.session/"latest-guard-copy";save(inject,library.string()+"\n"+(folder/primary.at("so").get<std::string>()).string()+"\n"+snapshot.string()+"\n"+(folder/"mpc-plugin.json").string()+"\n");
        const auto beforeGuard=checksumCalls(calls);blocked=false;try{manager.installLatestPlugin(item.at("id").get<std::string>(),repo);}catch(const std::exception&){blocked=true;}fs::remove(inject);
        check(blocked&&checksumCalls(calls)==beforeGuard+1&&!fs::exists(manager.session/"download.zip"),"installation arriving during latest check blocks package download");fs::remove_all(folder);
        bool installationReported=false;blocked=false;
        try{manager.installLatestPlugin(item.at("id").get<std::string>(),repo,[&](const std::string& stage){if(stage.find("Checking OS 3 compatibility")==0)installed(primary,folder);if(stage.find("Installing plugin")==0)installationReported=true;});}catch(const std::exception&){blocked=true;}
        check(blocked&&!installationReported&&sha256(folder/primary.at("so").get<std::string>())==sha256(library)&&!fs::exists(manager.synths/".mpc-usb-manager")&&!fs::exists(manager.usb/"plugin-history.json"),"fresh installed guard blocks installer execution after package validation");
        fs::remove_all(folder);fs::remove(snapshot);fs::remove(manager.session/"download.zip");fs::remove_all(manager.session/"install");
    } catch(...) {save(wget,originalWget);manager.catalog=catalog;throw;}
    save(wget,originalWget);manager.catalog=catalog;for(const auto& file:{response,status,calls})fs::remove(file);
}
int main(){try{
    const auto fixture=Json::parse(std::ifstream("/tmp/manager-fixtures/fixture.json"));
    const auto primary=fixture.at("primary");const auto id=primary.at("id").get<std::string>();
    Manager manager("/tmp/manager-usb","/tmp/manager-session","/sdcard/Expansions","/tmp/manager-settings.xml","armv7");
    const auto registry=manager.synths/".mpc-usb-manager"/id;
    check(!manager.darkTheme(),"light theme is default without saved preferences");manager.setDarkTheme(true);
    Manager reopened(manager.usb,manager.session,manager.synths,manager.settings,"armv7");check(reopened.darkTheme(),"dark theme persists across manager instances");manager.setDarkTheme(false);check(!reopened.darkTheme(),"light theme toggle persists");
    {std::ofstream prefs(manager.usb/"manager-preferences.json");prefs<<"{\"schema\":1,\"dark_theme\":false}";}
    check(!manager.hideWelcome(),"welcome shown by default with legacy theme preferences");manager.setHideWelcome(true);
    check(reopened.hideWelcome(),"hidden welcome persists across instances");manager.setDarkTheme(true);check(reopened.hideWelcome(),"theme change preserves hidden welcome");manager.setHideWelcome(false);check(reopened.darkTheme()&&!reopened.hideWelcome(),"welcome preference preserves theme and can be restored");manager.setDarkTheme(false);
    {std::ofstream prefs(manager.usb/"manager-preferences.json");prefs<<"{\"schema\":1,\"dark_theme\":false,\"hide_welcome\":\"bad\"}";}
    bool badPreference=false;try{manager.hideWelcome();}catch(const std::exception&){badPreference=true;}check(badPreference,"invalid welcome preference rejected");fs::remove(manager.usb/"manager-preferences.json");
    manager.openCatalog();check(manager.catalog.size()==1,"catalog stable plugin parsed");
    check(manager.catalog[0].value("author","")==primary.value("author",""),"author metadata joins matching version and checksum");
    managerReleaseChecks(manager);
    managerStageChecks(manager);
    bool rejected=false;try{manager.openCatalog();}catch(const std::exception&){rejected=true;}
    check(rejected,"catalog fetched once per session");
    auto local=manager.list("local");check(local.size()==1,"valid local ZIP listed; invalid ZIP excluded");
    check(local[0].at("id")==id,"local manifest identity matches supplied package");
    const auto supplied=fs::directory_iterator(manager.session/("local-"+local[0].at("sha").get<std::string>()))->path();
    latestReleaseChecks(manager,primary,fixture.at("older_version"),supplied/primary.at("folder").get<std::string>()/primary.at("so").get<std::string>());
    taggedReleaseChecks(manager);
    std::vector<std::string> stages;manager.install(local[0],[&](const std::string& text){stages.push_back(text);});check(stages.size()==4&&stages[2].find("compatibility")!=std::string::npos&&stages[3].find("Installing")!=std::string::npos,"validation precedes installation status");check(manager.list("installed").size()==1,"local install appears in installed view");
    check(fs::exists(registry/"uninstall.sh"),"persistent uninstall record retained");
    const auto folder=manager.synths/local[0].at("skin").get<std::string>();const bool skinManifest=fs::is_regular_file(folder/"mpc-plugin.json");
    if(skinManifest)fs::rename(folder/"mpc-plugin.json",manager.session/"primary-skin-manifest.saved");
    check(manager.list("installed").size()==1,"installed package without skin manifest found through manager registry");
    check(manager.list("catalog")[0].value("installed",false),"catalog card reflects actual installed package without skin manifest");
    if(skinManifest)fs::rename(manager.session/"primary-skin-manifest.saved",folder/"mpc-plugin.json");
    auto catalogCopy=manager.catalog;
    if(fixture.at("newer_version").is_string()){manager.catalog[0]["version"]=fixture.at("newer_version");check(manager.list("catalog")[0].value("update_available",false),"newer catalog version offers update");}
    else std::cout<<"SKIP: newer-version comparison requires a supported numeric release version\n";
    if(fixture.at("older_version").is_string()){manager.catalog[0]["version"]=fixture.at("older_version");check(!manager.list("catalog")[0].value("update_available",true),"older catalog version never offered as update");}
    else std::cout<<"SKIP: older-version comparison requires a lower supported numeric release version\n";
    manager.catalog=catalogCopy;
    manager.install(local[0]);check(manager.list("installed").size()==1,"reinstall remains one plugin");
    manager.remove(manager.list("installed")[0]);check(manager.list("installed").empty(),"local uninstall removes plugin");
    std::vector<float> amounts;manager.install(manager.catalog[0],{},[&](float amount){amounts.push_back(amount);});check(manager.list("installed").size()==1,"catalog download verified and installed");
    bool partial=false;for(const float amount:amounts)if(amount>0&&amount<0.99f)partial=true;check(partial&&amounts.back()==1,"download progress measures received bytes and completes after installation");
    manager.remove(manager.list("installed")[0]);check(manager.list("installed").empty(),"catalog-installed plugin uninstalled offline");
    manager.install(local[0]);fs::remove_all(registry);
    manager.remove(manager.list("installed")[0]);check(manager.list("installed").empty(),"existing plugin uninstalled using matching USB ZIP");
    const auto saved=Json::parse(std::ifstream(manager.usb/"plugin-history.json"));
    check(saved.at("plugins").at(id).at("installed")==false,"uninstall retains saved plugin history");
    check(manager.list("history").size()==1&&!manager.list("history")[0].value("installed",true),"wiped device can compare saved history against current catalog");
    manager.install(manager.list("history")[0]);check(manager.list("installed").size()==1,"saved plugin reinstalled through verified catalog");
    check(manager.list("history")[0].value("installed_version","")==local[0].at("version"),"saved view reports version from actual device manifest");
    manager.remove(manager.list("installed")[0]);
    auto originalCatalog=manager.catalog;manager.catalog.clear();
    check(manager.list("history")[0].value("origin","")=="local","saved plugin resolves to matching local ZIP offline");
    fs::rename(manager.usb/"local-plugins",manager.usb/"hidden-local");
    check(!manager.list("history")[0].value("blocked_reason","").empty(),"unavailable saved plugin stays visible with reinstall blocked");
    fs::rename(manager.usb/"hidden-local",manager.usb/"local-plugins");manager.catalog=originalCatalog;
    auto changedLocal=local[0];const auto changedZip=manager.session/"changed-local.zip";fs::copy_file(local[0].at("path").get<std::string>(),changedZip);changedLocal["path"]=changedZip.string();{std::ofstream file(changedZip,std::ios::binary|std::ios::app);file.put('x');}
    rejected=false;try{manager.install(changedLocal);}catch(const std::exception&){rejected=true;}fs::remove(changedZip);check(rejected&&manager.list("installed").empty(),"selected local ZIP rehashed before installation despite prior listing");
    {std::ofstream file(manager.usb/"plugin-history.json");file<<"damaged record";}
    rejected=false;try{manager.install(manager.catalog[0]);}catch(const std::exception&){rejected=true;}
    check(rejected&&manager.list("installed").empty(),"damaged USB history preserved and installation blocked before changes");
    {std::ofstream file(manager.usb/"plugin-history.json");file<<saved.dump();}
    auto wrong=manager.catalog[0];wrong["sha"]=std::string(64,'0');rejected=false;
    stages.clear();try{manager.install(wrong,[&](const std::string& text){stages.push_back(text);});}catch(const std::exception& e){rejected=std::string(e.what()).find("Not installed; plugin files unchanged.")==0;}check(stages.size()==4&&stages[2].find("Downloading")!=std::string::npos,"download rejection never reports installation");check(rejected&&manager.list("installed").empty(),"bad catalog checksum changes no plugin files");
    auto incompatible=manager.catalog[0];incompatible["os_compat"]=Json::array({"2.x"});stages.clear();fs::remove(manager.session/"download.zip");rejected=false;
    try{manager.install(incompatible,[&](const std::string& text){stages.push_back(text);});}catch(const std::exception& e){rejected=std::string(e.what()).find("Download blocked")!=std::string::npos;}
    check(rejected&&stages.size()==1&&!fs::exists(manager.session/"download.zip"),"OS2 catalog entry blocked before download");
    incompatible=manager.catalog[0];incompatible.erase("os_compat");rejected=false;try{manager.install(incompatible);}catch(const std::exception&){rejected=true;}check(rejected&&!fs::exists(manager.session/"download.zip"),"unknown catalog compatibility blocked before download");
    const auto extracted=fs::directory_iterator(fs::path("/tmp/manager-session")/("local-"+local[0].at("sha").get<std::string>()))->path();
    const auto calls=manager.session/"checksum-calls";
    const auto checksumWrapper=fs::path("/tmp/manager-test-bin/sha256sum");
    save(checksumWrapper,"#!/bin/sh\nprintf '%s\\n' called >>/tmp/manager-session/checksum-calls\nexec /usr/bin/busybox.nosuid sha256sum \"$@\"\n");fs::permissions(checksumWrapper,fs::perms::owner_all);
    fs::remove(calls);manager.manifest(extracted);check(checksumCalls(calls)==1,"all package files verified in one checksum process");
    const auto checksumFile=extracted/"SHA256SUMS";const auto checksums=text(checksumFile);const auto firstEnd=checksums.find('\n');
    if(firstEnd==std::string::npos)throw std::runtime_error("Supplied package has no complete checksum line");
    auto rejectChecksums=[&](const std::string& value,const char* name){save(checksumFile,value);fs::remove(calls);bool invalid=false;try{manager.manifest(extracted);}catch(const std::exception&){invalid=true;}save(checksumFile,checksums);check(invalid&&checksumCalls(calls)==0,name);};
    auto invalidChecksums=checksums;invalidChecksums[64]=':';rejectChecksums(invalidChecksums,"invalid checksum separator rejected before hashing");
    rejectChecksums(checksums+checksums.substr(0,firstEnd+1),"duplicate checksum rejected before hashing");
    rejectChecksums(checksums+std::string(64,'0')+"  ../outside\n","late traversal checksum rejected before any hashing");
    rejectChecksums(checksums+std::string(64,'0')+"  /tmp/manager-settings.xml\n","absolute checksum rejected before hashing");
    fs::copy_file(extracted/"mpc-plugin.json",extracted/"-");rejectChecksums(checksums+sha256(extracted/"-")+"  -\n","stdin checksum filename rejected before hashing");fs::remove(extracted/"-");
    fs::create_symlink(manager.settings,extracted/"checksum-escape");rejectChecksums(checksums+std::string(64,'0')+"  checksum-escape\n","checksum symlink escape rejected before hashing");fs::remove(extracted/"checksum-escape");
    rejectChecksums(checksums.substr(firstEnd+1),"unchecked payload rejected before hashing");
    const auto verifiedLibrary=extracted/primary.at("folder").get<std::string>()/primary.at("so").get<std::string>();const auto librarySize=fs::file_size(verifiedLibrary);
    {std::ofstream file(verifiedLibrary,std::ios::binary|std::ios::app);file.put('x');}
    fs::remove(calls);rejected=false;try{manager.manifest(extracted);}catch(const std::exception&){rejected=true;}fs::resize_file(verifiedLibrary,librarySize);check(rejected&&checksumCalls(calls)==1,"batched verifier rejects tampered payload");
    manager.manifest(extracted);
    std::ifstream originalFile(extracted/"mpc-plugin.json");const std::string original{std::istreambuf_iterator<char>(originalFile),{}};
    auto changed=Json::parse(original);changed["max_glibc"]="99.0";{std::ofstream out(extracted/"mpc-plugin.json");out<<changed.dump();}
    rejected=false;try{manager.manifest(extracted);}catch(const std::exception&){rejected=true;}check(rejected,"newer firmware library requirement rejected");
    changed=Json::parse(original);changed["os_compat"]=Json::array({"2.x"});{std::ofstream out(extracted/"mpc-plugin.json");out<<changed.dump();}
    rejected=false;try{manager.manifest(extracted);}catch(const std::exception&){rejected=true;}check(rejected,"OS2-only package rejected on OS3 manager");
    changed=Json::parse(original);changed.erase("os_compat");{std::ofstream out(extracted/"mpc-plugin.json");out<<changed.dump();}
    rejected=false;try{manager.manifest(extracted,&manager.catalog[0]);}catch(const std::exception& e){rejected=std::string(e.what()).find("checksum")!=std::string::npos;}check(rejected,"catalog compatibility fallback still enforces package checksums");
    {std::ofstream out(extracted/"mpc-plugin.json");out<<original;}
    for(const char* file:{"traversal.zip","absolute.zip","escape-link.zip","multi-root.zip","oversize.zip"}) {
        rejected=false;try{manager.unpack(fs::path("/tmp/manager-fixtures")/file,fs::path("/tmp")/(std::string("reject-")+file));}catch(const std::exception&){rejected=true;}
        check(rejected,file);
    }
    check(!fs::exists("/tmp/escape"),"archive attacks wrote nothing outside extraction root");
    check(fs::exists("/tmp/manager-session/catalog.tsv")&&!fs::exists("/tmp/manager-usb/catalog.tsv"),"catalog only in RAM session");
    if(!fixture.at("legacy").is_null()){
        const auto legacyCatalog=fixture.at("legacy");const auto legacyId=legacyCatalog.at("id").get<std::string>();
        const auto legacy=manager.unpack("/tmp/manager-fixtures/legacy.zip","/tmp/manager-session/legacy");
        check(!Json::parse(std::ifstream(legacy/"mpc-plugin.json")).contains("os_compat"),"supplied legacy manifest omits optional OS field");
        const auto legacyManifest=manager.manifest(legacy,&legacyCatalog);check(legacyManifest.at("id")==legacyId,"supplied legacy package passes checksum and confirmed catalog compatibility validation");
        manager.catalog.push_back(legacyCatalog);manager.install(legacyCatalog);
        const auto legacyFolder=manager.synths/legacyCatalog.at("skin").get<std::string>();const bool legacySkinManifest=fs::is_regular_file(legacyFolder/"mpc-plugin.json");
        if(legacySkinManifest)fs::rename(legacyFolder/"mpc-plugin.json",manager.session/"legacy-skin-manifest.saved");
        check(manager.list("installed").size()==1&&manager.list("installed")[0].at("installed_version")==legacyCatalog.at("version"),"supplied legacy package detected with release version from registry");
        bool legacyInstalled=false;for(const auto& item:manager.list("catalog"))if(item.at("id")==legacyId)legacyInstalled=item.value("installed",false);check(legacyInstalled,"supplied legacy catalog card marked installed from device");
        if(legacySkinManifest)fs::rename(manager.session/"legacy-skin-manifest.saved",legacyFolder/"mpc-plugin.json");
        manager.remove(manager.list("installed")[0]);check(manager.list("installed").empty(),"supplied legacy package uninstalled through its recorded installer");manager.catalog.pop_back();
    }else std::cout<<"SKIP: legacy compatibility installation requires a supplied legacy package and explicit OS 3 confirmation\n";
    {std::ofstream mounts("/proc/mounts");mounts<<"/dev/mmcblk1p1 /sdcard ext4 rw,noexec 0 0\n";}
    rejected=false;try{manager.install(local[0]);}catch(const std::exception&){rejected=true;}check(rejected&&manager.list("installed").empty(),"noexec SD-card rejection before mutation");
    {std::ofstream mounts("/proc/mounts");mounts<<"/dev/root / ext4 ro 0 0\n";}
    rejected=false;try{manager.install(local[0]);}catch(const std::exception&){rejected=true;}check(rejected&&manager.list("installed").empty(),"missing SD-card rejection before mutation");
    const auto library=extracted/primary.at("folder").get<std::string>()/primary.at("so").get<std::string>();
    if(!fs::is_regular_file(library))throw std::runtime_error("Supplied package library is missing from its declared folder");
    const auto external=manager.session/"registered"/fs::path(primary.at("so").get<std::string>()).filename();fs::create_directories(external.parent_path());fs::copy_file(library,external);
    {std::ofstream file(manager.settings);file<<"<PROPERTIES><VALUE name=\"pluginList-arm\"><KNOWNPLUGINS><PLUGIN name=\""<<xmlEscape(primary.at("name").get<std::string>())<<"\" file=\""<<xmlEscape(external.string())<<"\" uid=\""<<xmlEscape(primary.at("uid").get<std::string>())<<"\" isInstrument=\""<<(primary.at("kind")=="instrument"?"1":"0")<<"\" version=\""<<xmlEscape(primary.at("version").get<std::string>())<<"\"/></KNOWNPLUGINS></VALUE></PROPERTIES>";}
    check(manager.list("installed").size()==1&&manager.list("installed")[0].at("name")==primary.at("name"),"supplied plugin registered outside SD target found without package manifest");
    fs::rename(external,external.string()+".withheld");check(manager.list("installed").empty(),"registered plugin with missing binary excluded");fs::rename(external.string()+".withheld",external);
    rejected=false;try{manager.remove(manager.list("installed")[0]);}catch(const std::exception&){rejected=true;}check(rejected&&fs::exists(external),"external plugin cannot be removed using managed SD installer");fs::remove(external);
    const auto secondExternal=manager.session/"registered-two"/external.filename();fs::create_directories(secondExternal.parent_path());fs::copy_file(library,external);fs::copy_file(library,secondExternal);
    {std::ofstream file(manager.settings);file<<"<PROPERTIES><VALUE name=\"pluginList-arm\"><KNOWNPLUGINS>";int uid=1;for(const auto& path:{external,secondExternal})file<<"<PLUGIN name=\""<<xmlEscape(primary.at("name").get<std::string>())<<"\" file=\""<<xmlEscape(path.string())<<"\" uid=\""<<uid++<<"\" isInstrument=\"1\" version=\""<<xmlEscape(primary.at("version").get<std::string>())<<"\"/>";file<<"</KNOWNPLUGINS></VALUE></PROPERTIES>";}
    manager.catalog.clear();fs::remove(calls);manager.list("local");const auto oneScan=checksumCalls(calls);check(oneScan>1,"local scan includes supplied ZIP hash and batch verification");
    fs::remove(calls);const auto externalPlugins=manager.list("installed");check(externalPlugins.size()==2&&checksumCalls(calls)==oneScan,"multiple installed plugins scan local offers only once");
    const auto previousHistory=manager.list("history").size();
    for(const auto& item:externalPlugins)manager.remember(item,true);
    fs::remove(calls);const auto listedHistory=manager.list("history");check(listedHistory.size()==previousHistory+externalPlugins.size(),"history retains all supplied-package and device entries");check(checksumCalls(calls)==oneScan,"multiple history entries scan local offers only once");
    fs::remove(external);fs::remove(secondExternal);fs::remove(checksumWrapper);fs::remove(calls);save(manager.usb/"plugin-history.json",saved.dump());
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
