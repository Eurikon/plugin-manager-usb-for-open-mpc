#include "backend.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <regex>
static void check(bool ok,const char* name){if(!ok)throw std::runtime_error(name);std::cout<<"PASS: "<<name<<'\n';}
static std::string text(const fs::path& path){std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read test file");return {std::istreambuf_iterator<char>(file),{}};}
static void save(const fs::path& path,const std::string& value){std::ofstream file(path);file<<value;if(!file)throw std::runtime_error("Cannot write test file");}
static size_t checksumCalls(const fs::path& path){const auto value=fs::exists(path)?text(path):std::string();return std::count(value.begin(),value.end(),'\n');}
static std::string xmlEscape(const std::string& value){std::string result;for(const char c:value){switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;case '"':result+="&quot;";break;case '\'':result+="&apos;";break;default:result+=c;}}return result;}
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
    Manager manager("/tmp/manager-usb","/tmp/manager-session","/sdcard/Expansions","/tmp/manager-settings.xml");
    const auto registry=manager.synths/".mpc-usb-manager"/id;
    check(!manager.darkTheme(),"light theme is default without saved preferences");manager.setDarkTheme(true);
    Manager reopened(manager.usb,manager.session,manager.synths,manager.settings);check(reopened.darkTheme(),"dark theme persists across manager instances");manager.setDarkTheme(false);check(!reopened.darkTheme(),"light theme toggle persists");
    {std::ofstream prefs(manager.usb/"manager-preferences.json");prefs<<"{\"schema\":1,\"dark_theme\":false}";}
    check(!manager.hideWelcome(),"welcome shown by default with legacy theme preferences");manager.setHideWelcome(true);
    check(reopened.hideWelcome(),"hidden welcome persists across instances");manager.setDarkTheme(true);check(reopened.hideWelcome(),"theme change preserves hidden welcome");manager.setHideWelcome(false);check(reopened.darkTheme()&&!reopened.hideWelcome(),"welcome preference preserves theme and can be restored");manager.setDarkTheme(false);
    {std::ofstream prefs(manager.usb/"manager-preferences.json");prefs<<"{\"schema\":1,\"dark_theme\":false,\"hide_welcome\":\"bad\"}";}
    bool badPreference=false;try{manager.hideWelcome();}catch(const std::exception&){badPreference=true;}check(badPreference,"invalid welcome preference rejected");fs::remove(manager.usb/"manager-preferences.json");
    manager.openCatalog();check(manager.catalog.size()==1,"catalog stable plugin parsed");
    check(manager.catalog[0].value("author","")==primary.value("author",""),"author metadata joins matching version and checksum");
    bool rejected=false;try{manager.openCatalog();}catch(const std::exception&){rejected=true;}
    check(rejected,"catalog fetched once per session");
    auto local=manager.list("local");check(local.size()==1,"valid local ZIP listed; invalid ZIP excluded");
    check(local[0].at("id")==id,"local manifest identity matches supplied package");
    const auto supplied=fs::directory_iterator(manager.session/("local-"+local[0].at("sha").get<std::string>()))->path();
    latestReleaseChecks(manager,primary,fixture.at("older_version"),supplied/primary.at("folder").get<std::string>()/primary.at("so").get<std::string>());
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
