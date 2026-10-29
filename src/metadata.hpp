#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace pmu {
inline nlohmann::json parseMetadata(const std::string& text){
    using Json=nlohmann::json;std::map<int,std::set<std::string>> keys;
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value){
        if(depth>16)throw std::runtime_error("Metadata nesting exceeds its limit");
        if(event==Json::parse_event_t::object_start)keys[depth].clear();
        else if(event==Json::parse_event_t::key){if(!keys[depth-1].insert(value.get<std::string>()).second)throw std::runtime_error("Duplicate metadata key");}
        else if(event==Json::parse_event_t::object_end)keys.erase(depth);
        return true;
    });
}
}
