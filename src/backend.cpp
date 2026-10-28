#include "backend.hpp"
#include "version.hpp"
#include <archive.h>
#include <gnu/libc-version.h>
#include <algorithm>
#include <cstdio>
#include <archive_entry.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <set>
#include <regex>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <csignal>
#include <ctime>
#include <cctype>
#include <array>

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static std::string read(const fs::path& path) {
    std::ifstream file(path);
    require(bool(file), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
static void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path); file << text;
    require(bool(file), "Cannot write " + path.string());
}
static bool inside(const fs::path& root, const fs::path& child) {
    auto a = root.lexically_normal(), b = child.lexically_normal();
    auto ai=a.begin(), bi=b.begin();
    for (; ai!=a.end(); ++ai,++bi) if (bi==b.end() || *ai!=*bi) return false;
    return true;
}
static bool safeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part:path) if (part==".." || part==".") return false;
    return true;
}
int runProcess(const std::vector<std::string>& args, const std::function<void()>& tick, uint64_t maxFileBytes) {
    require(!args.empty(), "Empty process request");
    require(maxFileBytes>0&&maxFileBytes<=512ULL*1024*1024,"Invalid process file size limit");
    std::vector<char*> argv;
    for (const auto& arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid=fork(); require(pid>=0,"Cannot start process");
    if (pid==0) {
        setpgid(0,0);
        struct rlimit limit{static_cast<rlim_t>(maxFileBytes),static_cast<rlim_t>(maxFileBytes)};
        if(setrlimit(RLIMIT_FSIZE,&limit))_exit(126);
        execvp(argv[0],argv.data()); _exit(127);
    }
    setpgid(pid,pid);
    int status=0;
    const auto stop=[&](){kill(-pid,SIGTERM);std::this_thread::sleep_for(std::chrono::milliseconds(500));kill(-pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);
    for (;;) {
        const auto result=waitpid(pid,&status,WNOHANG);
        if(result==pid) break;
        if(result<0&&errno!=EINTR) throw std::runtime_error("Cannot wait for process");
        if(std::chrono::steady_clock::now()>deadline) {
            stop();
            throw std::runtime_error("Operation timed out; see USB log");
        }
        try{if(tick)tick();}catch(...){stop();throw;}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}
std::string sha256(const fs::path& file) {
    int pipes[2]; require(pipe(pipes)==0,"Cannot create checksum pipe");
    pid_t pid=fork();
    if(pid<0) {close(pipes[0]);close(pipes[1]);throw std::runtime_error("Cannot start checksum");}
    if(pid==0) {close(pipes[0]);dup2(pipes[1],1);close(pipes[1]);execlp("sha256sum","sha256sum",file.c_str(),nullptr);_exit(127);}
    close(pipes[1]);std::string output;char block[1024];ssize_t count;
    while((count=::read(pipes[0],block,sizeof block))>0) output.append(block,count);
    close(pipes[0]);int status=0;while(waitpid(pid,&status,0)<0) if(errno!=EINTR) throw std::runtime_error("Checksum wait failed");
    require(WIFEXITED(status)&&WEXITSTATUS(status)==0&&output.size()>=64,"Checksum failed");
    return output.substr(0,64);
}
static std::string xmlText(std::string text);
static std::string mountPath(const std::string& encoded) {
    std::string result;
    for(size_t i=0;i<encoded.size();++i) {
        if(encoded[i]=='\\') {
            require(i+3<encoded.size(),"Invalid mount path");
            const auto code=encoded.substr(i+1,3);
            require(code=="040"||code=="134","Unsupported mount path");
            result+=code=="040"?' ':'\\';i+=3;
        }else result+=encoded[i];
    }
    return result;
}
static bool executableContent(const fs::path& target,const std::string& mounts) {
    const std::string path=target.lexically_normal().string();
    bool allowed=false;
    for(const char* root:{"/nvme/","/sdcard/","/storage/","/data/UserData/","/media/"})
        if(path.rfind(root,0)==0)allowed=true;
    if(!allowed||!fs::is_directory(target))return false;
    std::istringstream input(mounts);std::string line;size_t longest=0;bool writable=false;
    while(std::getline(input,line)) {
        std::istringstream row(line);std::string device,point,type,options;
        if(!(row>>device>>point>>type>>options))continue;
        point=mountPath(point);
        if(point=="/"||!(path==point||path.rfind(point+"/",0)==0)||point.size()<longest)continue;
        longest=point.size();options=","+options+",";
        writable=options.find(",rw,")!=std::string::npos&&options.find(",noexec,")==std::string::npos&&options.find(",ro,")==std::string::npos;
    }
    return longest!=0&&writable;
}
Manager::Manager(fs::path u,fs::path s,fs::path target,fs::path set,std::string architecture,PluginStoragePaths storagePaths):usb(std::move(u)),session(std::move(s)),synths(std::move(target)),settings(std::move(set)),arch(std::move(architecture)),pluginStorage(settings,std::move(storagePaths)) {
    require(arch=="armv7"||arch=="aarch64","Unsupported device architecture");
    if(synths=="auto"){
        require(arch=="aarch64","Automatic plugin storage requires Gen 2");nativePluginStorage=true;synths.clear();
        try{selectPluginStorage({});}catch(const std::exception& error){pluginStorageError=error.what();}
    }
    fs::create_directories(session);
    require(setenv("MPC_SETTINGS",settings.c_str(),1)==0,"Cannot set settings path");
}
Json Manager::pluginStorageInfo() {
    Json result={{"available",nativePluginStorage},{"automatic",automaticPluginStorage},{"path",synths.string()},{"reason",pluginStorageError},{"targets",Json::array()}};
    if(!nativePluginStorage)return result;
    try{result["native"]=pluginStorage.automatic();}catch(const std::exception& error){result["native_error"]=error.what();}
    result["targets"]=pluginStorage.targets();return result;
}
Json Manager::pluginStorageFolders(const fs::path& directory) {require(nativePluginStorage,"Plugin location selection requires Gen 2 automatic storage");auto result=pluginStorageInfo();result["browser"]=pluginStorage.folders(directory);return result;}
void Manager::selectPluginStorage(const fs::path& target) {
    require(nativePluginStorage,"Plugin location selection requires Gen 2 automatic storage");
    const auto state=target.empty()?pluginStorage.automatic():pluginStorage.inspect(target);
    synths=state.at("path").get<std::string>();pluginStorageIdentity=state.at("identity").get<std::string>();automaticPluginStorage=target.empty();pluginStorageError.clear();
}
static std::vector<int> libcVersion(const std::string& text) {
    std::istringstream parts(text);std::string part;std::vector<int> result;
    while(std::getline(parts,part,'.')){require(!part.empty()&&part.find_first_not_of("0123456789")==std::string::npos,"Invalid libc version");result.push_back(std::stoi(part));}
    require(!result.empty(),"Missing libc version");while(result.size()<3)result.push_back(0);return result;
}
static std::string catalogBlock(const Json& item) {
    if(!item.value("available_for_arch",true))return "No package is available for this device generation";
    const auto compatibility=item.value("os_compat",Json::array());
    if(!compatibility.is_array()||std::find(compatibility.begin(),compatibility.end(),"3.x")==compatibility.end())return "Catalog does not confirm MPC OS 3 support";
    try {if(libcVersion(item.value("max_glibc",""))>libcVersion(gnu_get_libc_version()))return "Requires newer firmware libraries";}catch(const std::exception&){return "Catalog library compatibility is unknown";}
    if(!item.value("defer",false))return "Package installer cannot run while this manager is open";
    return {};
}
void Manager::openCatalog() {
    require(catalog.empty()&&!fs::exists(session/"catalog-attempted"),"Catalog already fetched this session");
    write(session/"catalog-attempted","1");
    const auto path=session/"catalog.tsv";
    if(runProcess({"wget","-T","15","-O",path.string(),"https://sd88me.github.io/mpc-vst-plugins/catalog.tsv"})!=0) {
        fs::remove(path);catalogError="Catalog unavailable. Local packages still work.";return;
    }
    std::istringstream lines(read(path));std::string line;
    if(!std::getline(lines,line)||line!="#mpc-catalog-tsv 1") {fs::remove(path);catalogError="Invalid catalog response";return;}
    while(std::getline(lines,line)) {
        std::vector<std::string> c;std::stringstream row(line);std::string field;
        while(std::getline(row,field,'\t')) c.push_back(field);
        if(c.size()<14||c[0]!="plugin"||c[3]!="1"||c[4]=="addin") continue;
        if(!std::regex_match(c[1],std::regex("[a-z0-9][a-z0-9_-]*"))) continue;
        const size_t asset=arch=="aarch64"?16:9;
        const bool available=c.size()>asset+2&&std::regex_match(c[asset],std::regex("[1-9][0-9]*"))
            &&std::regex_match(c[asset+1],std::regex("[0-9a-f]{64}"))&&c[asset+2].rfind("https://",0)==0;
        if(!available&&c[10]!="-"&&!std::regex_match(c[10],std::regex("[0-9a-f]{64}")))continue;
        Json compatibility=Json::array();
        if(c.size()>14){std::stringstream generations(c[14]);std::string generation;while(std::getline(generations,generation,','))if(generation!="-")compatibility.push_back(generation);}
        catalog.push_back({{"id",c[1]},{"version",c[2]},{"kind",c[4]},{"name",c[5]},{"skin",c[6]},{"uid",c[7]},{"size",available?c[asset]:"0"},{"sha",available?c[asset+1]:""},{"url",available?c[asset+2]:""},{"available_for_arch",available},{"arch",arch},{"origin","catalog"},{"os_compat",compatibility},{"max_glibc",c.size()>15?c[15]:""},{"defer",c[13]=="1"}});
        catalog.back()["blocked_reason"]=catalogBlock(catalog.back());
    }
    const auto details=session/"catalog.json";
    if(runProcess({"wget","-T","15","-O",details.string(),"https://sd88me.github.io/mpc-vst-plugins/catalog.json"})==0) {
        try {
            const auto metadata=Json::parse(read(details));
            require(metadata.value("schema",0)==1&&metadata.at("plugins").is_array(),"Invalid catalog metadata");
            for(const auto& plugin:metadata.at("plugins")) {
                if(!plugin.is_object()||!plugin.contains("versions")||!plugin.at("versions").is_array())continue;
                for(auto& item:catalog) {
                    if(plugin.value("id","")!=item.at("id"))continue;
                    for(const auto& version:plugin.at("versions")) {
                        if(!version.is_object()||version.value("version","")!=item.at("version"))continue;
                        const Json* asset=&version;
                        if(arch=="aarch64") {
                            if(!version.contains("assets")||!version.at("assets").is_object()
                                ||!version.at("assets").contains(arch)||!version.at("assets").at(arch).is_object())continue;
                            asset=&version.at("assets").at(arch);
                        }
                        if(asset->value("sha256","")!=item.at("sha"))continue;
                        for(const char* field:{"author","summary","license"})if(plugin.contains(field)&&plugin.at(field).is_string())item[field]=plugin.at(field);
                    }
                }
            }
        } catch(const std::exception& e){std::fprintf(stderr,"Catalog details unavailable: %s\n",e.what());}
    }
    else std::fprintf(stderr,"Catalog details unavailable; showing verified TSV entries\n");
}
static std::array<unsigned long,3> managerReleaseVersion(const std::string& tag) {
    std::smatch parts;
    require(std::regex_match(tag,parts,std::regex("v(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})")),"Manager release has an invalid stable version");
    return {std::stoul(parts[1].str()),std::stoul(parts[2].str()),std::stoul(parts[3].str())};
}
static Json managerReleaseAsset(const Json& release,const std::string& tag) {
    require(release.contains("assets")&&release.at("assets").is_array()&&release.at("assets").size()<=256,"No supported manager ZIP is published");
    const auto name="plugin-manager-usb-"+tag+".zip";
    const Json* candidate=nullptr;
    for(const auto& asset:release.at("assets")) {
        require(asset.is_object(),"Invalid manager release assets");
        if(!asset.contains("name")||asset.at("name")!=name)continue;
        require(candidate==nullptr,"The manager release has duplicate ZIP assets");candidate=&asset;
    }
    require(candidate!=nullptr,"No supported manager ZIP is published");
    const auto url="https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/download/"+tag+"/"+name;
    require(candidate->contains("state")&&candidate->at("state")=="uploaded"&&candidate->contains("browser_download_url")&&candidate->at("browser_download_url")==url,"The manager ZIP is unfinished or has an unexpected download URL");
    require(candidate->contains("size")&&candidate->at("size").is_number_unsigned()&&candidate->at("size").get<uint64_t>()>0&&candidate->at("size").get<uint64_t>()<=64ULL*1024*1024,"The manager ZIP size is unsupported");
    require(candidate->contains("digest")&&candidate->at("digest").is_string(),"The manager ZIP has no verified GitHub checksum");
    const auto digest=candidate->at("digest").get<std::string>();
    require(std::regex_match(digest,std::regex("sha256:[0-9a-f]{64}")),"The manager ZIP has no verified GitHub checksum");
    return {{"name",name},{"url",url},{"size",candidate->at("size")},{"sha256",digest.substr(7)}};
}
Json Manager::checkManagerUpdate() {
    const auto path=session/"manager-release.json";
    fs::remove(path);
    try {
        const auto status=runProcess({"wget","-T","15","-t","1","--header=Accept: application/vnd.github+json","-O",path.string(),"https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest"},{},1024*1024);
        require(status!=8,"GitHub rejected the manager update check (possibly rate limited); try again later");
        require(status==0,"Cannot check manager updates; check your Internet connection and try again");
        require(fs::is_regular_file(path)&&fs::file_size(path)>0&&fs::file_size(path)<=1024*1024,"Invalid manager release response");
        const auto release=Json::parse(read(path));
        require(release.is_object(),"Invalid manager release response");
        if(release.contains("message")&&release.at("message").is_string()) {
            require(release.at("message").get<std::string>().find("API rate limit exceeded")==std::string::npos,"GitHub rate limit reached; try the manager update check again later");
        }
        require(release.contains("draft")&&release.at("draft").is_boolean()&&!release.at("draft").get<bool>()&&release.contains("prerelease")&&release.at("prerelease").is_boolean()&&!release.at("prerelease").get<bool>(),"Manager release is not a stable published release");
        require(release.contains("tag_name")&&release.at("tag_name").is_string()&&release.contains("html_url")&&release.at("html_url").is_string(),"Invalid manager release response");
        const auto tag=release.at("tag_name").get<std::string>();
        const auto latest=managerReleaseVersion(tag),current=managerReleaseVersion(std::string("v")+MPC_MANAGER_VERSION);
        const auto url="https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/"+tag;
        require(release.at("html_url")==url,"Manager release URL does not match its repository and version");
        const bool available=latest>current,development=MPC_MANAGER_DEVELOPMENT;
        std::string message;
        if(development)message="Development build; latest published release is "+tag+(available?". A newer release is available.":". This local build is not a published release.");
        else if(available)message="Plugin Manager "+tag+" is available.";
        else if(latest==current)message="Plugin Manager is up to date.";
        else message="Installed version is newer than the latest published release ("+tag+").";
        Json asset=nullptr;std::string reason;
        try{asset=managerReleaseAsset(release,tag);}catch(const std::exception& error){reason=error.what();}
        if(available&&asset.is_null())message+=" "+reason+"; automatic update is unavailable.";
        fs::remove(path);
        return {{"current_version",MPC_MANAGER_VERSION},{"latest_version",tag.substr(1)},{"update_available",available},{"development",development},{"message",message},{"release_url",url},{"downloadable",!asset.is_null()},{"asset",asset}};
    } catch(const Json::exception&) {fs::remove(path);throw std::runtime_error("Invalid manager release response; try again later");}
    catch(...) {fs::remove(path);throw;}
}
static bool managerBundleFile(const std::string& name) {
    static const std::set<std::string> files={".mpc-manager-id","USB-SHA256.json","Plugin Manager helper.xpa","autorun/autorun.check","autorun/autorun.json","autorun/open.sh","autorun/runtime/manager","autorun/runtime/font.ttf","autorun/runtime/launch.sh","autorun/runtime/setup.sh","autorun/runtime/shortcut","autorun/runtime/shortcut-hook.so"};
    return files.count(name)||std::regex_match(name,std::regex("autorun/runtime/aarch64/(manager|shortcut|shortcut-hook\\.so)"))||std::regex_match(name,std::regex("autorun/runtime/licenses/[A-Za-z0-9][A-Za-z0-9_.-]*"));
}
static Json unpackManagerBundle(const fs::path& zip,const fs::path& destination) {
    require(!fs::exists(destination),"Manager staging folder already exists");fs::create_directory(destination);
    auto* archive=archive_read_new();require(archive!=nullptr,"Archive allocation failed");
    archive_read_support_format_zip(archive);
    try {
        require(archive_read_open_filename(archive,zip.c_str(),65536)==ARCHIVE_OK,"Cannot open manager ZIP");
        archive_entry* entry=nullptr;std::set<std::string> names,files;uint64_t total=0;int status;
        const std::set<std::string> directories={"autorun","autorun/runtime","autorun/runtime/aarch64","autorun/runtime/licenses","local-plugins"};
        while((status=archive_read_next_header(archive,&entry))==ARCHIVE_OK) {
            const char* raw=archive_entry_pathname(entry);require(raw!=nullptr,"Missing manager ZIP path");
            std::string name(raw);const auto type=archive_entry_filetype(entry);
            if(type==AE_IFDIR&&!name.empty()&&name.back()=='/')name.pop_back();
            const fs::path relative(name);
            require(name.size()<=240&&safeRelative(relative)&&relative.generic_string()==name&&names.insert(name).second&&names.size()<=256,"Unsafe or duplicate manager ZIP path");
            require(archive_entry_symlink(entry)==nullptr&&archive_entry_hardlink(entry)==nullptr&&!archive_entry_is_encrypted(entry),"Manager ZIP contains links or encrypted entries");
            require((type==AE_IFDIR&&directories.count(name))||(type==AE_IFREG&&managerBundleFile(name)),"Manager ZIP contains unsupported content");
            if(type==AE_IFDIR){require(archive_entry_size(entry)==0,"Invalid manager ZIP directory");fs::create_directories(destination/relative);continue;}
            const auto size=archive_entry_size(entry);require(size>0&&static_cast<uint64_t>(size)<=64ULL*1024*1024,"Unsupported manager ZIP file size");
            total+=static_cast<uint64_t>(size);require(total<=128ULL*1024*1024,"Manager ZIP exceeds its expanded size limit");
            if(name=="USB-SHA256.json"||name=="autorun/autorun.json")require(size<=128*1024,"Oversized manager bundle metadata");
            if(name.rfind("autorun/runtime/licenses/",0)==0)require(size<=1024*1024,"Oversized manager license");
            const auto target=destination/relative;fs::create_directories(target.parent_path());std::ofstream output(target,std::ios::binary);require(bool(output),"Cannot create staged manager file");
            std::array<char,65536> buffer;la_ssize_t count;uint64_t written=0;
            while((count=archive_read_data(archive,buffer.data(),buffer.size()))>0){written+=static_cast<uint64_t>(count);require(written<=static_cast<uint64_t>(size),"Manager ZIP size mismatch");output.write(buffer.data(),count);require(bool(output),"Cannot write staged manager file");}
            require(count==0&&written==static_cast<uint64_t>(size),"Manager ZIP is incomplete");output.close();require(bool(output),"Cannot finish staged manager file");files.insert(name);
        }
        require(status==ARCHIVE_EOF&&!files.empty(),"Manager ZIP is incomplete");archive_read_free(archive);archive=nullptr;
        require(files.count("USB-SHA256.json"),"Manager bundle has no file inventory");const auto manifest=Json::parse(read(destination/"USB-SHA256.json"));
        require(manifest.is_object()&&manifest.size()+1==files.size(),"Manager bundle inventory is incomplete");
        for(const auto& item:manifest.items())require(files.count(item.key())&&item.key()!="USB-SHA256.json"&&item.value().is_string()&&std::regex_match(item.value().get<std::string>(),std::regex("[0-9a-f]{64}"))&&sha256(destination/item.key())==item.value(),"Manager bundle file checksum mismatch");
        for(const char* name:{".mpc-manager-id","autorun/autorun.check","autorun/autorun.json","autorun/open.sh","autorun/runtime/manager","autorun/runtime/font.ttf","autorun/runtime/launch.sh","autorun/runtime/setup.sh","autorun/runtime/shortcut","autorun/runtime/shortcut-hook.so","autorun/runtime/licenses/plugin-manager-usb.txt","autorun/runtime/licenses/SDL2.txt","autorun/runtime/licenses/community-tools.txt","autorun/runtime/licenses/font-license.txt","autorun/runtime/licenses/json-license.txt"})require(files.count(name),"Manager bundle is missing a required runtime file");
        require(read(destination/".mpc-manager-id")=="mpc-usb-manager-v1\n","Manager bundle marker is invalid");
        require(std::regex_match(read(destination/"autorun/autorun.check"),std::regex("[0-9a-f]{64}")),"Manager autorun checksum is invalid");
        const auto autorun=Json::parse(read(destination/"autorun/autorun.json"));
        require(autorun.is_object()&&autorun.size()==1&&autorun.contains("value0")&&autorun.at("value0").is_object(),"Invalid manager autorun metadata");
        const auto& value=autorun.at("value0");require(value.contains("files")&&value.at("files").is_array()&&value.contains("scripts")&&value.at("scripts").is_array()&&value.at("scripts").size()==1&&value.at("scripts")[0].is_object()&&value.at("scripts")[0].contains("path")&&value.at("scripts")[0].at("path")=="open.sh","Invalid manager autorun entries");
        std::set<std::string> declared,expected;for(const auto& file:files)if(file.rfind("autorun/runtime/",0)==0)expected.insert(file.substr(8));
        for(const auto& file:value.at("files")){require(file.is_string()&&declared.insert(file.get<std::string>()).second,"Invalid manager autorun file inventory");}require(declared==expected,"Manager autorun inventory differs from its runtime");
        for(const auto& architecture:std::vector<std::string>{"armv7","aarch64"}) {
            const auto directory=destination/"autorun/runtime"/(architecture=="armv7"?"":"aarch64");
            if(architecture=="aarch64"&&!fs::exists(directory))continue;
            for(const char* name:{"manager","shortcut","shortcut-hook.so"}) {
                std::array<unsigned char,64> header{};std::ifstream binary(directory/name,std::ios::binary);
                binary.read(reinterpret_cast<char*>(header.data()),header.size());
                auto word=[&](size_t offset){return unsigned(header[offset])|(unsigned(header[offset+1])<<8);};
                const bool is64=architecture=="aarch64";
                require(binary.gcount()>=static_cast<std::streamsize>(is64?64:52)&&header[0]==0x7f&&header[1]=='E'&&header[2]=='L'&&header[3]=='F'&&header[4]==(is64?2:1)&&header[5]==1&&header[6]==1&&(word(16)==2||word(16)==3)&&word(18)==(is64?183:40)&&word(is64?52:40)==(is64?64:52),"Manager runtime is not a supported ARM ELF");
            }
        }
        fs::create_directories(destination/"local-plugins");return manifest;
    }catch(...){if(archive)archive_read_free(archive);std::error_code error;fs::remove_all(destination,error);throw;}
}
Json Manager::stageManagerUpdate(const Json& checkedRelease,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount) {
    require(checkedRelease.is_object()&&checkedRelease.contains("latest_version")&&checkedRelease.contains("release_url")&&checkedRelease.contains("asset"),"Check for a manager update before downloading it");
    if(progress)progress("Rechecking the latest manager release...");
    const auto release=checkManagerUpdate();
    require(release.at("update_available")==true,"No newer published manager release is available");
    require(release.at("downloadable")==true,"The latest manager release has no verified download");
    require(checkedRelease.at("latest_version")==release.at("latest_version")&&checkedRelease.at("release_url")==release.at("release_url")&&checkedRelease.at("asset")==release.at("asset"),"The manager release changed; check for updates again");
    std::string temporary=(session/"manager-update.XXXXXX").string();std::vector<char> name(temporary.begin(),temporary.end());name.push_back(0);
    const auto created=mkdtemp(name.data());require(created!=nullptr,"Cannot create manager update staging folder");const fs::path stage(created);
    try {
        const auto zip=stage/"manager.zip",bundle=stage/"bundle";const auto& asset=release.at("asset");const auto size=asset.at("size").get<uint64_t>();
        if(progress)progress("Downloading the verified manager release...");
        if(amount)amount(0);
        const auto status=runProcess({"wget","-T","15","-t","1","-O",zip.string(),asset.at("url").get<std::string>()},[&](){if(amount){std::error_code error;const auto bytes=fs::file_size(zip,error);if(!error)amount(std::min(0.8f,0.8f*float(bytes)/float(size)));}},size);
        require(status==0,"Manager download failed; check your connection and retry");
        require(fs::is_regular_file(zip)&&fs::file_size(zip)==size&&sha256(zip)==asset.at("sha256"),"Manager download size or checksum mismatch");
        if(progress)progress("Verifying the manager bundle...");
        const auto files=unpackManagerBundle(zip,bundle);
        require(arch!="aarch64"||fs::is_regular_file(bundle/"autorun/runtime/aarch64/manager"),"Manager update has no Gen 2 runtime");
        fs::remove(zip);if(amount)amount(1);
        return {{"bundle_root",bundle.string()},{"release",release},{"files",files}};
    }catch(...){std::error_code error;fs::remove_all(stage,error);throw;}
}
void Manager::verifyLatestPlugin(const std::string& id,const ReleaseSource& source) {
    const std::string& repo=source.repo;
    require(std::regex_match(repo,std::regex("[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9][A-Za-z0-9_.-]*")),"Invalid plugin repository");
    const Json* item=nullptr;
    for(const auto& entry:catalog)if(entry.value("id","")==id){require(item==nullptr,"Catalog contains duplicate plugin entries");item=&entry;}
    require(item!=nullptr,"Plugin is unavailable in the current catalog");
    const auto blocked=catalogBlock(*item);require(blocked.empty(),"Plugin unavailable: "+blocked);
    const auto prefix="https://github.com/"+repo+"/releases/download/";
    require(item->value("url","").rfind(prefix,0)==0,"Catalog plugin repository does not match");
    auto version=[](std::string value){if(!value.empty()&&value.front()=='v')value.erase(0,1);return value;};
    const auto catalogVersion=version(item->at("version").get<std::string>());
    const auto wantedTag=source.tagPrefix+catalogVersion;
    if(!source.tagPrefix.empty())require(std::regex_match(wantedTag,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]*")),"Catalog version is not usable in a release tag");
    const auto path=session/"latest-release.json";
    fs::remove(path);
    try {
        require(runProcess({"wget","-T","15","-t","1","--header=Accept: application/vnd.github+json","-O",path.string(),"https://api.github.com/repos/"+repo+"/releases/"+(source.tagPrefix.empty()?std::string("latest"):"tags/"+wantedTag)})==0,"Cannot check the latest release; try again when online");
        require(fs::is_regular_file(path)&&fs::file_size(path)<=1024*1024,"Invalid latest release response");
        const auto release=Json::parse(read(path));
        require(release.is_object()&&release.contains("draft")&&release.at("draft")==false&&release.contains("prerelease")&&release.at("prerelease")==false,"Latest release is not a stable published release");
        const auto tag=release.at("tag_name").get<std::string>();
        require(std::regex_match(tag,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]*"))&&(source.tagPrefix.empty()?version(tag)==catalogVersion:tag==wantedTag),"Catalog has not caught up with the latest release; try again later");
        const auto& assets=release.at("assets");require(assets.is_array()&&assets.size()<=1024,"Invalid latest release assets");
        const Json* asset=nullptr;
        for(const auto& entry:assets) {
            require(entry.is_object(),"Invalid latest release asset");
            const auto name=entry.value("name","");
            const auto suffix="-mpc-"+arch+".zip";
            if(name.size()<suffix.size()||name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0)continue;
            require(asset==nullptr,"Latest release has multiple ARMv7 packages");asset=&entry;
        }
        require(asset!=nullptr&&asset->value("state","")=="uploaded","Latest release has no ready ARMv7 package");
        const auto name=asset->at("name").get<std::string>();
        require(source.assetPrefix.empty()||name==source.assetPrefix+catalogVersion+"-mpc-"+arch+".zip","Release package name does not match this manager");
        require(std::regex_match(name,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]*"))&&asset->at("browser_download_url").get<std::string>()==prefix+tag+"/"+name&&asset->at("browser_download_url")==item->at("url"),"Latest package does not match the catalog download");
        const auto size=item->at("size").get<std::string>();
        require(std::regex_match(size,std::regex("[1-9][0-9]*"))&&asset->at("size").is_number_unsigned(),"Invalid latest package size");
        const auto bytes=std::stoull(size);require(bytes<=512ULL*1024*1024&&asset->at("size").get<uint64_t>()==bytes,"Latest package size does not match the catalog");
        const auto checksum=item->at("sha").get<std::string>();
        require(std::regex_match(checksum,std::regex("[0-9a-f]{64}"))&&asset->at("digest").get<std::string>()=="sha256:"+checksum,"Latest package checksum does not match the catalog");
        fs::remove(path);
    } catch(...) {fs::remove(path);throw;}
}
fs::path Manager::unpack(const fs::path& zip,const fs::path& destination) {
    require(!fs::exists(destination),"Extraction destination already exists");fs::create_directories(destination);
    struct archive* archive=archive_read_new();require(archive!=nullptr,"Archive allocation failed");
    archive_read_support_format_zip(archive);archive_read_support_filter_all(archive);
    try {
        require(archive_read_open_filename(archive,zip.c_str(),65536)==ARCHIVE_OK,"Cannot open ZIP");
        struct archive_entry* entry=nullptr;uint64_t total=0;size_t count=0;std::set<std::string> roots,names;
        std::vector<std::pair<fs::path,fs::path>> links;
        int result;
        while((result=archive_read_next_header(archive,&entry))==ARCHIVE_OK) {
            const char* raw=archive_entry_pathname(entry);require(raw!=nullptr,"Missing archive path");fs::path relative(raw);
            require(safeRelative(relative)&&names.insert(relative.string()).second,"Unsafe or duplicate ZIP path");
            roots.insert(relative.begin()->string());require(roots.size()==1&&++count<100000,"ZIP must contain one package folder");
            fs::path target=destination/relative;const auto type=archive_entry_filetype(entry);
            if(type==AE_IFDIR) {fs::create_directories(target);continue;}
            fs::create_directories(target.parent_path());
            if(type==AE_IFLNK) {
                const char* rawLink=archive_entry_symlink(entry);require(rawLink!=nullptr,"Invalid symlink");fs::path link(rawLink);
                require(!link.is_absolute()&&inside(destination/(*relative.begin()),target.parent_path()/link),"Symlink escapes package");
                links.emplace_back(target,link);continue;
            }
            require(type==AE_IFREG&&archive_entry_hardlink(entry)==nullptr,"Unsupported archive entry");
            require(archive_entry_size(entry)>=0,"Invalid entry length");
            total+=uint64_t(archive_entry_size(entry));require(total<=512ULL*1024*1024,"Package exceeds 512 MiB RAM limit");
            std::ofstream output(target,std::ios::binary);require(bool(output),"Cannot create extracted file");
            char buffer[65536];la_ssize_t n;uint64_t written=0;
            while((n=archive_read_data(archive,buffer,sizeof buffer))>0) {written+=n;require(written<=uint64_t(archive_entry_size(entry)),"ZIP size mismatch");output.write(buffer,n);require(bool(output),"Extraction write failed");}
            require(n==0&&written==uint64_t(archive_entry_size(entry)),"ZIP read failed");
        }
        require(result==ARCHIVE_EOF&&roots.size()==1,"ZIP is incomplete");
        for(const auto& link:links) fs::create_symlink(link.second,link.first);
        archive_read_free(archive);
        return destination/(*roots.begin());
    } catch(...) {archive_read_free(archive);fs::remove_all(destination);throw;}
}
Json Manager::manifest(const fs::path& package, const Json* catalogItem) {
    Json value=Json::parse(read(package/"mpc-plugin.json"));
    require(value.value("schema",0)==1&&value.value("arch","")==arch,"Unsupported package manifest");
    require(std::regex_match(value.at("id").get<std::string>(),std::regex("[a-z0-9][a-z0-9_-]*")),"Invalid plugin ID");
    require(value.value("kind","")!="addin","System addins are not supported by this manager");
    if(!value.contains("os_compat")&&catalogItem){require(catalogBlock(*catalogItem).empty(),"Catalog does not confirm MPC OS 3 support");value["os_compat"]=catalogItem->at("os_compat");}
    require(value.contains("os_compat")&&value.at("os_compat").is_array()&&std::find(value.at("os_compat").begin(),value.at("os_compat").end(),"3.x")!=value.at("os_compat").end(),"Package does not declare MPC OS 3 support");
    require(libcVersion(value.at("max_glibc").get<std::string>())<=libcVersion(gnu_get_libc_version()),"Plugin requires newer firmware libraries");
    require(safeRelative(value.at("folder").get<std::string>())&&safeRelative(value.at("skin").get<std::string>())&&fs::path(value.at("skin").get<std::string>()).filename()==value.at("skin").get<std::string>(),"Invalid plugin folder");
    for(const char* file:{"install.sh","uninstall.sh","plugin_list.awk","SHA256SUMS"}) require(fs::is_regular_file(package/file),std::string("Missing ")+file);
    require(read(package/"install.sh").find("DEFER=")!=std::string::npos&&read(package/"uninstall.sh").find("DEFER=")!=std::string::npos,"Package does not support a stopped MPC session");
    const auto root=fs::canonical(package);
    require(inside(root,fs::canonical(package/"SHA256SUMS")),"Checksum path escapes package");
    std::istringstream sums(read(package/"SHA256SUMS"));std::string line;std::set<std::string> covered;
    while(std::getline(sums,line)) {
        require(line.size()>66&&std::regex_match(line.substr(0,64),std::regex("[0-9a-f]{64}"))&&line[64]==' '&&(line[65]==' '||line[65]=='*'),"Invalid checksum file");
        std::string name=line.substr(66);fs::path relative(name);
        require(name!="-"&&std::none_of(name.begin(),name.end(),[](unsigned char c){return c<32;})&&name.find('\\')==std::string::npos&&relative.generic_string()==name&&safeRelative(relative)&&fs::is_regular_file(package/relative)&&inside(root,fs::canonical(package/relative)),"Checksum path escapes package");
        require(covered.insert(name).second,"Duplicate package checksum");
    }
    for(const auto& file:fs::recursive_directory_iterator(package))
        if(fs::is_regular_file(file.path())&&file.path()!=package/"SHA256SUMS") require(inside(root,fs::canonical(file.path()))&&covered.count(file.path().lexically_relative(package).string()),"Unchecked package file");
    require(runProcess({"sh","-c","cd \"$1\" && exec sha256sum -c SHA256SUMS >/dev/null","checksum",root.string()})==0,"Package checksum mismatch");
    return value;
}
static void atomicSave(const fs::path& target,const std::string& text) {
    const auto temporary=fs::path(target.string()+".tmp");write(temporary,text);
    const int file=open(temporary.c_str(),O_RDONLY|O_CLOEXEC);require(file>=0,"Cannot open USB record for sync");const int synced=fsync(file);close(file);require(synced==0,"Cannot sync USB record");
    fs::rename(temporary,target);
    const int directory=open(target.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(directory>=0,"Cannot open USB directory for sync");const int renamed=fsync(directory);close(directory);require(renamed==0,"Cannot sync USB record directory");
}
static Json preferences(const fs::path& usb) {
    const auto path=usb/"manager-preferences.json";
    if(!fs::exists(path))return {{"schema",1},{"dark_theme",false},{"hide_welcome",false}};
    const auto value=Json::parse(read(path));
    require(value.value("schema",0)==1&&value.at("dark_theme").is_boolean()&&(!value.contains("hide_welcome")||value.at("hide_welcome").is_boolean()),"Invalid saved preferences");
    return value;
}
bool Manager::darkTheme() {return preferences(usb).at("dark_theme").get<bool>();}
void Manager::setDarkTheme(bool dark) {auto value=preferences(usb);value["dark_theme"]=dark;atomicSave(usb/"manager-preferences.json",value.dump(2)+"\n");}
bool Manager::hideWelcome() {return preferences(usb).value("hide_welcome",false);}
void Manager::setHideWelcome(bool hidden) {auto value=preferences(usb);value["hide_welcome"]=hidden;atomicSave(usb/"manager-preferences.json",value.dump(2)+"\n");}
static Json history(const fs::path& usb) {
    const auto path=usb/"plugin-history.json";
    if(!fs::exists(path))return {{"schema",1},{"plugins",Json::object()}};
    const auto value=Json::parse(read(path));
    require(value.value("schema",0)==1&&value.at("plugins").is_object(),"USB installation record is invalid; preserve it before repairing");
    return value;
}
void Manager::remember(const Json& item,bool installed) {
    auto value=history(usb);const auto id=item.at("id").get<std::string>();
    require(std::regex_match(id,std::regex("[a-z0-9][a-z0-9_-]*")),"Invalid history identity");
    Json record=value.at("plugins").value(id,Json::object());
    for(const char* field:{"id","name","version","kind","skin","author","manufacturer","origin"})if(item.contains(field)&&item.at(field).is_string())record[field]=item.at(field);
    const auto now=std::time(nullptr);std::tm time{};gmtime_r(&now,&time);char stamp[32];std::strftime(stamp,sizeof stamp,"%d-%m-%Y %H:%M:%S UTC",&time);
    record["installed"]=installed;record["updated"]=stamp;value["plugins"][id]=record;
    atomicSave(usb/"plugin-history.json",value.dump(2)+"\n");
}
void Manager::recordInstalled() {
    const auto saved=history(usb);
    for(const auto& item:installedPlugins()) {
        const auto id=item.at("id").get<std::string>();
        if(!saved.at("plugins").contains(id)||saved.at("plugins").at(id).value("version","")!=item.at("version")||!saved.at("plugins").at(id).value("installed",false))remember(item,true);
    }
}
static std::string xmlText(std::string text) {
    for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"&quot;","\""},{"&apos;","'"},{"&lt;","<"},{"&gt;",">"},{"&amp;","&"}}){size_t at=0;while((at=text.find(pair.first,at))!=std::string::npos){text.replace(at,pair.first.size(),pair.second);at+=pair.second.size();}}return text;
}
std::vector<Json> Manager::installedPlugins() {
    std::vector<Json> result;std::set<std::string> files;
    auto append=[&](Json item,const fs::path& file,const fs::path& root){
        if(!file.is_absolute()||!fs::is_regular_file(file)||!files.insert(fs::canonical(file).string()).second)return;
        item["origin"]="installed";item["installed"]=true;item["installed_version"]=item.value("version","");item["device_file"]=file.string();item["device_target"]=fs::exists(root)&&fs::exists(synths)&&fs::equivalent(root,synths)?synths.string():root.string();
        item["managed"]=!synths.empty()&&fs::is_regular_file(synths/".mpc-usb-manager"/item.at("id").get<std::string>()/"uninstall.sh")&&item.at("device_target")==synths.string();
        result.push_back(item);
    };
    auto metadata=[&](const fs::path& file){
        try{auto item=Json::parse(read(file));const auto id=item.at("id").get<std::string>(),skin=item.at("skin").get<std::string>(),so=item.at("so").get<std::string>();
            if(item.value("arch",arch)!=arch)return;
            require(std::regex_match(id,std::regex("[a-z0-9][a-z0-9_-]*"))&&safeRelative(skin)&&fs::path(skin).filename()==skin&&safeRelative(so),"Invalid installed metadata");
            require(item.at("name").is_string()&&item.at("version").is_string(),"Invalid installed identity");if(item.value("kind","")=="addin")return;
            item["release_version_known"]=true;append(item,synths/skin/so,synths);
        }catch(const std::exception& e){std::fprintf(stderr,"Installed metadata: %s\n",e.what());}
    };
    if(fs::is_directory(synths))for(const auto& folder:fs::directory_iterator(synths))if(fs::is_directory(folder)&&folder.path().filename().string().front()!='.'&&fs::is_regular_file(folder.path()/"mpc-plugin.json"))metadata(folder.path()/"mpc-plugin.json");
    const auto registry=synths/".mpc-usb-manager";
    if(!synths.empty()&&fs::is_directory(registry))for(const auto& folder:fs::directory_iterator(registry))if(fs::is_directory(folder)&&fs::is_regular_file(folder.path()/"mpc-plugin.json"))metadata(folder.path()/"mpc-plugin.json");
    if(fs::is_regular_file(settings)){
        const auto xml=read(settings);const auto value=xml.find("<VALUE name=\"pluginList-"+std::string(arch=="aarch64"?"arm-64bit":"arm")+"\"");
        const auto begin=value==std::string::npos?std::string::npos:xml.find('>',value);const auto finish=begin==std::string::npos?std::string::npos:xml.find("</VALUE>",begin);
        if(finish!=std::string::npos){
            const std::string list=xml.substr(begin+1,finish-begin-1);const std::regex tag("<PLUGIN\\s+([^>]+)>");const std::regex attribute("([A-Za-z][A-Za-z0-9_-]*)\\s*=\\s*\"([^\"]*)\"");
            for(std::sregex_iterator it(list.begin(),list.end(),tag),end;it!=end;++it){const std::string text=(*it)[1];Json attrs=Json::object();for(std::sregex_iterator at(text.begin(),text.end(),attribute);at!=end;++at)attrs[(*at)[1].str()]=xmlText((*at)[2]);
                const fs::path file=attrs.value("file","");if(!file.is_absolute()||!fs::is_regular_file(file))continue;
                const auto skin=file.parent_path().filename().string();Json item;bool matched=false;
                for(const auto& entry:catalog)if((!attrs.value("uid","").empty()&&entry.value("uid","")==attrs.value("uid",""))||entry.value("skin","")==skin){item=entry;matched=true;break;}
                if(!matched){std::string id="device-"+attrs.value("uid",attrs.value("name",file.stem().string()));for(char& c:id){c=std::tolower(static_cast<unsigned char>(c));if(!std::isalnum(static_cast<unsigned char>(c))&&c!='-'&&c!='_')c='-';}item={{"id",id},{"skin",skin},{"name",attrs.value("name",file.stem().string())},{"kind",attrs.value("isInstrument","0")=="1"?"instrument":"effect"}};}
                item["version"]=attrs.value("version","");item["release_version_known"]=false;item["manufacturer"]=attrs.value("manufacturer","");item["uid"]=attrs.value("uid","");
                append(item,file,file.parent_path().parent_path());
            }
        }
    }
    return result;
}
Json Manager::installationOffer(const Json& item) {
    auto offer=[](const std::string& label,bool allowed,const std::string& reason){return Json{{"label",label},{"allowed",allowed},{"reason",reason}};};
    bool found=false;
    for(const auto& installed:installedPlugins()) {
        bool matches=false;
        for(const char* field:{"id","skin","uid"}){const auto value=item.value(field,"");if(!value.empty()&&installed.value(field,"")==value)matches=true;}
        if(!matches)continue;
        found=true;
        if(installed.value("device_target","")!=synths.string())return offer("Installed",false,"Already installed outside the managed content folder");
        if(!installed.value("release_version_known",false))return offer("Installed",false,"Already installed; its release version is unknown");
        auto version=[](std::string value){if(!value.empty()&&value.front()=='v')value.erase(0,1);return libcVersion(value);};
        try {if(version(installed.value("version",""))>=version(item.value("version","")))return offer("Installed",false,"The current or a newer version is already installed");}
        catch(const std::exception&){return offer("Installed",false,"Already installed; its release version cannot be compared");}
    }
    const auto blocked=catalogBlock(item);if(!blocked.empty())return offer("Unavailable",false,blocked);
    return offer(found?"Update":"Install",true,{});
}
void Manager::installLatestPlugin(const std::string& id,const ReleaseSource& source,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount) {
    const Json* selected=nullptr;
    for(const auto& item:catalog)if(item.value("id","")==id){require(selected==nullptr,"Catalog contains duplicate plugin entries");selected=&item;}
    require(selected!=nullptr,"Plugin is unavailable in the current catalog");
    const Json item=*selected;const auto offer=installationOffer(item);require(offer.at("allowed").get<bool>(),offer.at("reason").get<std::string>());
    verifyLatestPlugin(id,source);
    installPackage(item,progress,amount,true);
}
std::vector<Json> Manager::list(const std::string& view) {
    std::vector<Json> result,localOffers;bool localLoaded=false;
    auto local=[&]() -> const std::vector<Json>& {if(!localLoaded){localOffers=list("local");localLoaded=true;}return localOffers;};
    if(view=="catalog")result=catalog;
    else if(view=="installed")result=installedPlugins();
    else if(view=="history") {
        const auto saved=history(usb);
        for(const auto& entry:saved.at("plugins").items()){
            Json item=entry.value();if(!item.is_object()||!item.contains("id")||!item.at("id").is_string()||item.value("id","")!=entry.key()||!item.contains("name")||!item.at("name").is_string()||!item.contains("skin")||!item.at("skin").is_string())continue;
            const auto version=item.value("version","");bool available=false;
            for(const auto& candidate:catalog)if(candidate.at("id")==item.at("id")){item=candidate;available=true;break;}
            if(!available)for(const auto& candidate:local())if(candidate.at("id")==item.at("id")){item=candidate;available=true;break;}
            item["saved_version"]=version;if(!available)item["blocked_reason"]="Not available in the community catalog or local ZIPs";result.push_back(item);
        }
    }
    else if(view=="local"&&fs::is_directory(usb/"local-plugins")) {
        for(const auto& entry:fs::directory_iterator(usb/"local-plugins")) {
            if(entry.path().filename().string().front()=='.'||entry.path().extension()!=".zip"||!fs::is_regular_file(entry.path())) continue;
            const auto hash=sha256(entry.path());const auto destination=session/("local-"+hash);
            try {
                fs::path root;
                if(fs::exists(destination)) {root=fs::directory_iterator(destination)->path();}
                else root=unpack(entry.path(),destination);
                Json item=manifest(root);item["origin"]="local";item["path"]=entry.path().string();item["sha"]=hash;result.push_back(item);
            } catch(const std::exception& e) {std::fprintf(stderr,"Local package %s: %s\n",entry.path().filename().c_str(),e.what());}
        }
    }

    const auto actual=view=="installed"?result:installedPlugins();
    if(view=="installed")for(auto& item:result){bool offered=false;for(const auto& candidate:catalog)if(candidate.at("id")==item.at("id")){item=candidate;offered=true;break;}if(!offered){for(const auto& candidate:local())if(candidate.at("id")==item.at("id")){item=candidate;offered=true;break;}}if(!offered)item["blocked_reason"]="This plugin is managed by its original installer";}

    for(auto& item:result){item["installed"]=false;for(const auto& device:actual)if(device.at("id")==item.at("id")||device.at("skin")==item.at("skin")){item["installed"]=true;item["installed_version"]=device.value("version","");item["device_item"]=device;break;}}
    for(auto& item:result){item["update_available"]=false;if(item.value("installed",false)&&item.at("device_item").value("release_version_known",false)){try{item["update_available"]=libcVersion(item.value("version",""))>libcVersion(item.value("installed_version",""));}catch(const std::exception&){}}}
    std::sort(result.begin(),result.end(),[](const Json&a,const Json&b){return a.at("name").template get<std::string>()<b.at("name").template get<std::string>();});return result;
}
void Manager::validateTarget() {
    if(nativePluginStorage){
        if(automaticPluginStorage){const auto state=pluginStorage.automatic();require(synths==fs::path(state.at("path").get<std::string>())&&pluginStorageIdentity==state.at("identity"),"MPC content drive changed. Select Automatic again in Plugin installation location.");}
        require(!synths.empty(),pluginStorageError.empty()?"Choose a plugin installation location in SETUP.":pluginStorageError);
        require(pluginStorage.inspect(synths).at("identity")==pluginStorageIdentity,"Plugin drive was removed or replaced. Choose the location again.");return;
    }
    require(fs::is_directory(synths)&&fs::is_regular_file(settings),"Plugin content folder or settings missing");
    const auto mounts=read("/proc/mounts");std::istringstream lines(mounts);std::string line;bool valid=arch=="aarch64"&&executableContent(synths,mounts);
    while(std::getline(lines,line)) {std::istringstream row(line);std::string device,path,type,opts;row>>device>>path>>type>>opts;if(arch=="armv7"&&path=="/sdcard") {const auto options=","+opts+",";valid=options.find(",rw,")!=std::string::npos&&options.find(",noexec,")==std::string::npos&&options.find(",ro,")==std::string::npos;}}
    require(valid,"Plugin storage must be mounted writable and executable");
    require(read(settings).find("<Location>"+synths.string()+"</Location>")!=std::string::npos,"Plugin content location is not registered");
}
void Manager::install(const Json& item, const std::function<void(const std::string&)>& progress, const std::function<void(float)>& amount) {
    installPackage(item,progress,amount,false);
}
void Manager::installPackage(const Json& item,const std::function<void(const std::string&)>& progress,const std::function<void(float)>& amount,bool latestOnly) {
    bool started=false,installed=false;
    auto report=[&](const std::string& text){std::fprintf(stderr,"%s\n",text.c_str());if(progress)progress(text);};
    try {
    if(latestOnly){const auto offer=installationOffer(item);require(offer.at("allowed").get<bool>(),offer.at("reason").get<std::string>());}
    if(item.at("origin")=="catalog"){report("Checking catalog compatibility before download...");const auto reason=catalogBlock(item);require(reason.empty(),"Download blocked: "+reason);}
    report("Checking installation destination...");
    history(usb);validateTarget();fs::path zip;
    if(item.at("origin")=="catalog") {
        report("Downloading package. No plugin files changed...");
        zip=session/"download.zip";float size=0;try{size=std::stof(item.value("size","0"));}catch(const std::exception&){}
        auto tick=[&]{if(!amount)return;std::error_code ec;const auto bytes=fs::file_size(zip,ec);amount(!ec&&size>0?std::min(0.99f,float(bytes)/size):-1.f);};
        require(runProcess({"wget","-T","30","-O",zip.string(),item.at("url").get<std::string>()},tick)==0,"Download failed");
        if(amount)amount(-1);
    } else zip=item.at("path").get<std::string>();
    report("Checking package checksum...");
    require(sha256(zip)==item.at("sha").get<std::string>(),"ZIP changed or catalog checksum mismatch");
    const auto temporary=session/"install";fs::remove_all(temporary);const auto root=unpack(zip,temporary);
    report("Checking OS 3 compatibility and package contents...");
    const Json value=manifest(root,item.at("origin")=="catalog"?&item:nullptr);
    require(value.at("id")==item.at("id")&&value.at("version")==item.at("version")&&value.at("skin")==item.at("skin"),"Package identity differs from selection");
    if(latestOnly){const auto offer=installationOffer(item);require(offer.at("allowed").get<bool>(),offer.at("reason").get<std::string>());}
    const auto registry=synths/".mpc-usb-manager"/value.at("id").get<std::string>();
    report("Installing plugin to "+synths.string()+". Keep the manager drive inserted...");started=true;
    if(nativePluginStorage)pluginStorage.prepare(synths,pluginStorageIdentity);
    require(runProcess({"sh",(root/"install.sh").string(),"-y","-n","-t",synths.string()})==0,"Community package installer failed; see USB log");
    fs::create_directories(registry);
    for(const char* file:{"uninstall.sh","plugin_list.awk","mpc-plugin.json"}) fs::copy_file(root/file,registry/file,fs::copy_options::overwrite_existing);
    installed=true;remember(item,true);if(amount)amount(1);
    fs::remove_all(temporary);if(item.at("origin")=="catalog") fs::remove(zip);
    } catch(const std::exception& e) {const std::string message=std::string(installed?"Plugin installed, but the USB installation record could not be saved. ":started?"Installation failed; plugin files may have changed. ":"Not installed; plugin files unchanged. ")+e.what();std::fprintf(stderr,"%s\n",message.c_str());throw std::runtime_error(message);}
}
void Manager::remove(const Json& selection) {
    const Json item=selection.value("device_item",selection);
    require(item.value("device_target",synths.string())==synths.string(),"This plugin is installed outside the selected plugin location; its original installer must remove it");
    history(usb);validateTarget();const std::string id=item.at("id").get<std::string>();
    require(std::regex_match(id,std::regex("[a-z0-9][a-z0-9_-]*")),"Invalid plugin ID");
    const auto registry=synths/".mpc-usb-manager"/id;
    if(!fs::is_regular_file(registry/"uninstall.sh")) {
        bool found=false;
        for(const auto& local:list("local")) if(local.at("id")==item.at("id")&&local.at("version")==item.at("version")&&local.at("skin")==item.at("skin")) {
            const auto root=fs::directory_iterator(session/("local-"+local.at("sha").get<std::string>()))->path();
            fs::create_directories(registry);for(const char* file:{"uninstall.sh","plugin_list.awk","mpc-plugin.json"})fs::copy_file(root/file,registry/file,fs::copy_options::overwrite_existing);found=true;break;
        }
        require(found,"Uninstall requires the matching local ZIP or a manager installation record");
    }
    const auto value=Json::parse(read(registry/"mpc-plugin.json"));require(value.at("id")==item.at("id")&&value.at("skin")==item.at("skin")&&value.at("version")==item.at("version"),"Uninstall record differs from installed package");
    require(runProcess({"sh",(registry/"uninstall.sh").string(),"-y","-n","-t",synths.string()})==0,"Community uninstaller failed; see USB log");fs::remove_all(registry);
    try{remember(item,false);}catch(const std::exception& e){throw std::runtime_error(std::string("Plugin uninstalled, but the USB installation record could not be saved. ")+e.what());}
}
