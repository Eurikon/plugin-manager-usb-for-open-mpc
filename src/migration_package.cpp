#include "migration_package.hpp"
#include "metadata.hpp"
#include <stdexcept>

namespace pmu {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string relativeName(const Json& value){
    const auto name=value.get<std::string>();GenerationSpec probe{"path-check","universal",{{name,std::string(64,'0')}}};(void)generationSpecToJson(probe);return name;
}
MigrationPayload payload(const UpdateBundle& bundle,const Json& recipe,const std::string& architecture,const std::string& kind){
    need(recipe.is_object()&&recipe.size()==3,"Invalid update payload description");const auto directory=relativeName(recipe.at("directory")),manifest=relativeName(recipe.at("manifest")),approval=relativeName(recipe.at("approval"));
    const auto metadata=parseMetadata(readUpdateFile(bundle,manifest));const auto spec=generationSpecFromJson(metadata);need(metadata==generationSpecToJson(spec)&&spec.version==bundle.spec.version&&(spec.architecture==architecture||spec.architecture=="universal"),"Update component version or architecture differs from its release");
    for(const auto& file:spec.files){const auto expected=fs::path(directory)/file.first;const auto found=bundle.spec.files.find(expected.generic_string());need(found!=bundle.spec.files.end()&&found->second==file.second,"Update component is not covered by the complete bundle inventory");}
    const auto& roles=spec.entrypoints;
    if(kind=="runtime"){
        for(const auto& role:{"runtime_worker_"+architecture,"manager_"+architecture,"supervisor_"+architecture,"hook_"+architecture,std::string("launch"),std::string("setup"),std::string("font")})need(roles.count(role),"Update runtime lacks a required role");
    }else need(roles.count("helper_"+architecture)&&roles.count("bootstrap"),"Update helper lacks a required role");
    return {bundle.directory/directory,spec,readUpdateFile(bundle,approval,8192)};
}
}
MigrationPackage readMigrationPackage(const UpdateBundle& bundle,const std::string& architecture){
    need(architecture=="armv7"||architecture=="aarch64","Unsupported update architecture");const auto role=bundle.spec.entrypoints.find("deployment");need(role!=bundle.spec.entrypoints.end(),"Release has no migration description");const auto recipe=parseMetadata(readUpdateFile(bundle,role->second));
    need(recipe.is_object()&&recipe.size()==4&&recipe.value("schema",0)==1&&recipe.value("product",std::string())=="plugin-manager-usb","Unsupported migration description");MigrationPackage result{payload(bundle,recipe.at("runtime"),architecture,"runtime"),{}};
    if(!recipe.at("helper").is_null())result.helper=payload(bundle,recipe.at("helper"),architecture,"helper");
    return result;
}
}
