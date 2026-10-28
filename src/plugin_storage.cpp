#include "plugin_storage.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void need(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string read(const fs::path& path){std::ifstream file(path);need(bool(file),"Cannot read "+path.string());return {std::istreambuf_iterator<char>(file),{}};}
std::string decode(std::string value){
    for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"&quot;","\""},{"&apos;","'"},{"&lt;","<"},{"&gt;",">"},{"&amp;","&"}}){size_t at=0;while((at=value.find(pair.first,at))!=std::string::npos){value.replace(at,pair.first.size(),pair.second);at+=pair.second.size();}}
    return value;
}
std::string encode(const std::string& value){std::string result;for(char c:value){if(c=='&')result+="&amp;";else if(c=='<')result+="&lt;";else if(c=='>')result+="&gt;";else if(c=='\"')result+="&quot;";else result+=c;}return result;}
struct Value {size_t begin,end;std::string tag;};
// libstdc++ regex matching exhausts the stack on MPC's large settings attributes.
std::map<std::string,std::string> tagAttributes(const std::string& text){
    std::map<std::string,std::string> attrs;
    auto space=[&](size_t at){while(at<text.size()&&std::isspace(static_cast<unsigned char>(text[at])))++at;return at;};
    auto nameChar=[](char c){return std::isalnum(static_cast<unsigned char>(c))||c=='_'||c=='.'||c=='-';};
    for(size_t at=0;at<text.size();){
        if(!std::isalpha(static_cast<unsigned char>(text[at]))){++at;continue;}
        size_t end=at;while(end<text.size()&&nameChar(text[end]))++end;
        size_t cursor=space(end);
        if(cursor<text.size()&&text[cursor]=='='){
            cursor=space(cursor+1);
            if(cursor<text.size()&&(text[cursor]=='"'||text[cursor]=='\'')){
                const size_t close=text.find(text[cursor],cursor+1);
                if(close!=std::string::npos){need(attrs.emplace(text.substr(at,end-at),decode(text.substr(cursor+1,close-cursor-1))).second,"Duplicate settings attribute");at=close+1;continue;}
            }
        }
        ++at;
    }
    return attrs;
}
Value property(const std::string& xml,const std::string& name){
    Value result{std::string::npos,0,{}};
    for(size_t from=0;(from=xml.find("<VALUE",from))!=std::string::npos;){
        const size_t after=from+6;
        if(after<xml.size()&&(std::isalnum(static_cast<unsigned char>(xml[after]))||xml[after]=='_')){from=after;continue;}
        const size_t gt=xml.find('>',after);if(gt==std::string::npos)break;
        const std::string text=xml.substr(from,gt-from+1);
        const auto attrs=tagAttributes(text);
        const auto named=attrs.find("name");
        if(named==attrs.end()||named->second!=name){from=gt+1;continue;}
        need(result.begin==std::string::npos,"Duplicate "+name+" setting");
        const auto start=from,close=text.size()>1&&text[text.size()-2]=='/'?start+text.size():xml.find("</VALUE>",start+text.size());
        need(close!=std::string::npos,"Invalid "+name+" setting");
        result={start,text.size()>1&&text[text.size()-2]=='/'?close:close+8,text};
        from=gt+1;
    }
    return result;
}
std::string contentDrive(const std::string& xml){
    const auto value=property(xml,"Content.Manager.Directory");need(value.begin!=std::string::npos,"MPC content download drive is not configured. Choose a plugin installation location in SETUP.");
    const std::regex attribute("\\bval\\s*=\\s*([\"'])(.*?)\\1");std::smatch match;
    need(std::regex_search(value.tag,match,attribute)&&!match[2].str().empty(),"MPC content download drive is empty. Choose a plugin installation location in SETUP.");return decode(match[2]);
}
std::vector<fs::path> registered(const std::string& xml){
    const auto value=property(xml,"SynthContentLocations");std::vector<fs::path> result;if(value.begin==std::string::npos)return result;
    const auto section=xml.substr(value.begin,value.end-value.begin);const std::regex location("<Location>\\s*([^<]*?)\\s*</Location>");
    for(std::sregex_iterator it(section.begin(),section.end(),location),end;it!=end;++it)result.emplace_back(fs::path(decode((*it)[1])).lexically_normal());
    return result;
}
bool under(const fs::path& root,const fs::path& path){auto a=root.begin(),b=path.begin();for(;a!=root.end();++a,++b)if(b==path.end()||*a!=*b)return false;return true;}
void plain(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path,"Plugin location must be a normalized absolute path");
    need(path.string().find_first_of("&|\\\r\n")==std::string::npos,"Plugin installer does not support special characters in this location");
    fs::path part;for(const auto& name:path){part/=name;struct stat st{};if(lstat(part.c_str(),&st)){need(errno==ENOENT,"Cannot inspect plugin location");continue;}need(!S_ISLNK(st.st_mode),"Plugin location contains a symbolic link");}
}
dev_t device(const PluginStoragePaths& paths,const fs::path& path){if(paths.device)return paths.device(path);struct stat st{};need(lstat(path.c_str(),&st)==0,"Content drive is missing or disconnected: "+path.string());return st.st_dev;}
std::string unescape(const std::string& value){std::string out;for(size_t i=0;i<value.size();++i){if(value[i]!='\\'){out+=value[i];continue;}need(i+3<value.size(),"Invalid mounted drive path");const auto code=value.substr(i+1,3);need(code=="040"||code=="011"||code=="012"||code=="134","Invalid mounted drive escape");out+=char(std::stoi(code,nullptr,8));i+=3;}return out;}
struct Mount {fs::path point;std::string id,options,source,root;dev_t dev;};
std::vector<Mount> mounts(const PluginStoragePaths& paths){
    std::vector<Mount> result;std::istringstream input(read(paths.mounts));std::string line;
    while(std::getline(input,line)){
        std::istringstream row(line);std::string id,parent,number,root,point,options,field,type,source,super;
        if(!(row>>id>>parent>>number>>root>>point>>options))continue;
        while(row>>field&&field!="-"){}if(field!="-"||!(row>>type>>source>>super))continue;
        unsigned majorValue,minorValue;char tail;need(sscanf(number.c_str(),"%u:%u%c",&majorValue,&minorValue,&tail)==2,"Invalid mounted drive identity");
        result.push_back({unescape(point),id,","+options+","+super+",",unescape(source),unescape(root),makedev(majorValue,minorValue)});
    }
    return result;
}
bool userDrive(const Mount& mount){
    if(mount.source.rfind("/dev/",0)!=0||mount.root!="/")return false;
    for(const char* path:{"/boot","/data","/dev","/etc","/proc","/run","/sys","/usr","/var","/media/az01-internal"})if(under(path,mount.point))return false;
    return mount.point!="/";
}
Mount containing(const PluginStoragePaths& paths,const fs::path& path){
    const auto list=mounts(paths);const Mount* selected=nullptr;
    for(const auto& mount:list)if(under(mount.point,path)&&(!selected||mount.point.string().size()>=selected->point.string().size()))selected=&mount;
    need(selected!=nullptr&&userDrive(*selected),"Plugin location is not on a mounted content drive: "+path.string());
    for(const auto& mount:list)if(mount.point=="/")need(mount.dev!=selected->dev,"Plugin location resolves to the system filesystem");
    need(selected->options.find(",rw,")!=std::string::npos&&selected->options.find(",ro,")==std::string::npos,"Plugin drive is read-only: "+selected->point.string());
    need(selected->options.find(",noexec,")==std::string::npos,"Plugin drive does not permit executable plugins (noexec): "+selected->point.string());
    plain(selected->point);need(fs::is_directory(selected->point)&&device(paths,selected->point)==selected->dev,"Plugin drive is no longer mounted: "+selected->point.string());return *selected;
}
fs::path syncedCopy(const fs::path& path,const std::string& text,const struct stat& attributes){
    const auto pattern=path.string()+".pmu-XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    const int fd=mkstemp(name.data());need(fd>=0,"Cannot create settings backup");bool ok=fchown(fd,attributes.st_uid,attributes.st_gid)==0&&fchmod(fd,attributes.st_mode&0777)==0;
    size_t offset=0;
    while(ok&&offset<text.size()){const auto count=::write(fd,text.data()+offset,text.size()-offset);if(count<0&&errno==EINTR)continue;if(count<=0)ok=false;else offset+=size_t(count);}
    if(fsync(fd))ok=false;
    if(close(fd))ok=false;
    if(!ok){unlink(name.data());throw std::runtime_error("Cannot save and sync MPC settings");}
    return name.data();
}
void saveSettings(const fs::path& path,const std::string& before,const std::string& after){
    plain(path);struct stat st{};need(lstat(path.c_str(),&st)==0&&S_ISREG(st.st_mode),"MPC settings are missing");
    syncedCopy(path,before,st);const auto replacement=syncedCopy(path,after,st);
    try{
        struct stat current{};need(lstat(path.c_str(),&current)==0&&current.st_dev==st.st_dev&&current.st_ino==st.st_ino&&read(path)==before,"MPC settings changed during destination preparation");
        fs::rename(replacement,path);
        const int directory=open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);need(directory>=0,"Cannot sync settings folder");const int status=fsync(directory);close(directory);need(status==0,"Cannot sync settings folder");
    }catch(...){std::error_code ec;fs::remove(replacement,ec);throw;}
}
}
PluginStorage::PluginStorage(fs::path set,PluginStoragePaths p):settings(std::move(set)),paths(std::move(p)){}
Json PluginStorage::inspect(const fs::path& target) const{
    plain(target);const auto mount=containing(paths,target);const auto xml=read(settings);const auto locations=registered(xml);
    const bool exists=fs::exists(target);need(!exists||fs::is_directory(target),"Plugin location is occupied by a file");
    const bool known=std::any_of(locations.begin(),locations.end(),[&](const fs::path& path){return path==target||(exists&&fs::exists(path)&&fs::equivalent(path,target));});
    need(target!=mount.point&&(exists||target==mount.point/"Synths"),"Choose an existing folder or the drive's Synths folder");
    plain(exists?target:target.parent_path());need(device(paths,exists?target:target.parent_path())==mount.dev,"Plugin location changed filesystem");
    return {{"path",target.string()},{"drive",mount.point.string()},{"identity",mount.id+":"+std::to_string(mount.dev)},{"registered",known},{"exists",exists}};
}
Json PluginStorage::folders(const fs::path& directory) const{
    plain(directory);const auto mount=containing(paths,directory);need(fs::is_directory(directory)&&device(paths,directory)==mount.dev,"Plugin folder is missing or on a different filesystem");
    std::vector<std::string> entries;
    for(const auto& entry:fs::directory_iterator(directory)){if(entry.path().filename().string().front()=='.'||entry.is_symlink()||!entry.is_directory())continue;if(entries.size()>=2048)throw std::runtime_error("Too many folders in this location");try{plain(entry.path());const auto child=containing(paths,entry.path());if(child.id==mount.id)entries.push_back(entry.path().string());}catch(const std::exception&) {}}
    std::sort(entries.begin(),entries.end());return {{"path",directory.string()},{"parent",directory==mount.point?"":directory.parent_path().string()},{"selectable",directory!=mount.point},{"folders",entries}};
}
Json PluginStorage::automatic() const{
    const fs::path selected=contentDrive(read(settings));plain(selected);const auto dev=device(paths,selected);
    const auto mount=containing(paths,selected);need(mount.dev==dev,"MPC content drive does not match its mounted filesystem");return inspect(mount.point/"Synths");
}
std::vector<Json> PluginStorage::targets() const{
    std::vector<fs::path> candidates=registered(read(settings));for(const auto& mount:mounts(paths))if(userDrive(mount))candidates.push_back(mount.point/"Synths");
    std::vector<Json> result;std::set<std::pair<dev_t,std::string>> seen;
    for(const auto& target:candidates){try{const auto item=inspect(target);const auto root=fs::path(item.at("drive").get<std::string>());if(seen.emplace(device(paths,root),target.lexically_relative(root).string()).second)result.push_back(item);}catch(const std::exception&) {}}
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b){return a.at("path").get<std::string>()<b.at("path").get<std::string>();});return result;
}
void PluginStorage::prepare(const fs::path& target,const std::string& identity) const{
    auto state=inspect(target);need(state.at("identity")==identity,"Plugin drive was removed or replaced. Choose the location again.");
    if(!state.at("exists").get<bool>())need(mkdir(target.c_str(),0755)==0,"Cannot create plugin content folder");
    state=inspect(target);need(state.at("identity")==identity,"Plugin drive changed during preparation");if(state.at("registered").get<bool>())return;
    const auto before=read(settings);std::string after=before;const auto value=property(before,"SynthContentLocations");
    const std::string location="<Location>"+encode(target.string())+"</Location>";
    if(value.begin==std::string::npos){const auto end=after.rfind("</PROPERTIES>");need(end!=std::string::npos,"Invalid MPC settings document");after.insert(end,"<VALUE name=\"SynthContentLocations\"><SynthContentLocations>"+location+"</SynthContentLocations></VALUE>\n");}
    else{const auto section=before.substr(value.begin,value.end-value.begin);const auto end=section.find("</SynthContentLocations>");if(end!=std::string::npos)after.insert(value.begin+end,location);else{need(section.find("<SynthContentLocations/>")!=std::string::npos||section.find("<SynthContentLocations />")!=std::string::npos||value.tag.find("/>")!=std::string::npos,"Invalid SynthContentLocations setting");after.replace(value.begin,value.end-value.begin,"<VALUE name=\"SynthContentLocations\"><SynthContentLocations>"+location+"</SynthContentLocations></VALUE>");}}
    saveSettings(settings,before,after);
}
