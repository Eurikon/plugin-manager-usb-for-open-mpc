#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <charconv>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace pmu {
inline bool requiresUserRom(const nlohmann::json& item){
    const auto known=item.find("requires_user_rom");if(known!=item.end()&&known->is_boolean()&&known->get<bool>())return true;
    const auto files=item.find("user_files");if(files==item.end()||!files->is_array())return false;
    for(const auto& file:*files){
        if(!file.is_object())continue;
        const auto label=file.find("label");if(label==file.end()||!label->is_string())continue;
        auto text=label->get<std::string>();if(text.empty())continue;
        for(char& c:text)if(static_cast<unsigned char>(c)<128)c=char(std::tolower(static_cast<unsigned char>(c)));
        if(text.find("(optional)")==std::string::npos)return true;
    }
    return false;
}
inline std::string downloadSizeLabel(const nlohmann::json& item){
    const auto field=item.find("size");if(field==item.end())return {};
    std::string text;
    if(field->is_string())text=field->get<std::string>();
    else if(field->is_number_unsigned()||field->is_number_integer())text=field->dump();
    else return {};
    uint64_t bytes=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),bytes);
    if(parsed.ec!=std::errc()||parsed.ptr!=text.data()+text.size()||!bytes)return {};
    const char* units[]={"B","KB","MB","GB","TB","PB","EB"};
    double amount=double(bytes);int unit=0;while(amount>=1000&&unit<6){amount/=1000;++unit;}
    std::ostringstream label;label<<std::fixed<<std::setprecision(unit?1:0)<<amount<<' '<<units[unit];return label.str();
}
struct CatalogTabs {
    static constexpr int gap=12;
    int end;
    int left(int index)const{return index==3?648:28+(end-28-3*gap)*index/4+index*gap;}
    int right(int index)const{return index==2?632:28+(end-28-3*gap)*(index+1)/4+index*gap;}
    int hit(int x)const{for(int i=0;i<4;++i)if(x>=left(i)&&x<right(i))return i;return -1;}
};
inline int wheelCard(int selected,int page,int count,int delta){
    if(count<=0||delta==0)return selected;
    if(selected<0||selected>=count)return std::min(std::max(page,0)*int64_t(6)+(delta<0?5:0),int64_t(count-1));
    return int(std::clamp(int64_t(selected)+delta,int64_t(0),int64_t(count-1)));
}
struct CardTap {
    int previous=-1;
    uint32_t when=0;
    void clear(){previous=-1;}
    bool open(int index,uint32_t now){
        const bool twice=index==previous&&uint32_t(now-when)<=450;
        previous=twice?-1:index;when=now;return twice;
    }
};
struct BrowseOptions {
    std::string query,kind;
    bool updatesOnly=false,compatibleOnly=false;
    enum class Order {Name,Reverse,Updates};
    Order order=Order::Name;
};
inline std::map<std::string,BrowseOptions> decodeBrowseSettings(const nlohmann::json& value){
    if(!value.is_object()||value.size()>4)throw std::runtime_error("Invalid saved filter and sort settings");
    std::map<std::string,BrowseOptions> result;
    for(const auto& entry:value.items()){
        if(entry.key()!="catalog"&&entry.key()!="local"&&entry.key()!="installed"&&entry.key()!="history")throw std::runtime_error("Unknown saved browsing tab");
        const auto& saved=entry.value();
        if(!saved.is_object()||saved.size()!=4||!saved.at("kind").is_string()||!saved.at("updates_only").is_boolean()||!saved.at("compatible_only").is_boolean()||!saved.at("sort").is_string())throw std::runtime_error("Invalid saved filter and sort settings");
        auto& options=result[entry.key()];options.kind=saved.at("kind");options.updatesOnly=saved.at("updates_only");options.compatibleOnly=saved.at("compatible_only");const auto order=saved.at("sort").get<std::string>();
        if(options.kind!=""&&options.kind!="instrument"&&options.kind!="effect")throw std::runtime_error("Unknown saved plugin type");
        if(order=="name")options.order=BrowseOptions::Order::Name;else if(order=="reverse")options.order=BrowseOptions::Order::Reverse;else if(order=="updates")options.order=BrowseOptions::Order::Updates;else throw std::runtime_error("Unknown saved sort order");
    }
    return result;
}
inline nlohmann::json encodeBrowseSettings(const std::map<std::string,BrowseOptions>& options){
    auto value=nlohmann::json::object();
    for(const auto& entry:options){const auto& selected=entry.second;value[entry.first]={{"kind",selected.kind},{"updates_only",selected.updatesOnly},{"compatible_only",selected.compatibleOnly},{"sort",selected.order==BrowseOptions::Order::Name?"name":selected.order==BrowseOptions::Order::Reverse?"reverse":"updates"}};}
    decodeBrowseSettings(value);return value;
}
inline std::string searchText(std::string value){for(char& c:value)if(static_cast<unsigned char>(c)<128)c=char(std::tolower(static_cast<unsigned char>(c)));return value;}
inline void eraseSearchCharacter(std::string& value){if(value.empty())return;size_t at=value.size()-1;while(at>0&&(static_cast<unsigned char>(value[at])&0xc0)==0x80)--at;value.erase(at);}
inline std::vector<nlohmann::json> browsePlugins(const std::vector<nlohmann::json>& source,const BrowseOptions& options){
    std::vector<nlohmann::json> result;std::vector<std::string> words;std::istringstream query(searchText(options.query));std::string word;while(query>>word)words.push_back(word);
    for(const auto& item:source){
        if(!options.kind.empty()&&item.value("kind","")!=options.kind)continue;
        if(options.updatesOnly&&!item.value("update_available",false))continue;
        if(options.compatibleOnly&&(!item.value("blocked_reason","").empty()||!item.value("available_for_arch",true)))continue;
        std::string text;for(const char* field:{"name","id","author","manufacturer","summary"})text+=' '+item.value(field,"");text=searchText(std::move(text));
        if(std::any_of(words.begin(),words.end(),[&](const std::string& token){return text.find(token)==std::string::npos;}))continue;
        result.push_back(item);
    }
    std::stable_sort(result.begin(),result.end(),[&](const auto& a,const auto& b){
        if(options.order==BrowseOptions::Order::Updates&&a.value("update_available",false)!=b.value("update_available",false))return a.value("update_available",false);
        const auto left=searchText(a.value("name","")),right=searchText(b.value("name",""));
        return options.order==BrowseOptions::Order::Reverse?left>right:left<right;
    });
    return result;
}
}
