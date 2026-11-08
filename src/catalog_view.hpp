#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace pmu {
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
