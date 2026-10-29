#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <nlohmann/json_fwd.hpp>

namespace pmu {
struct GenerationSpec {
    std::string version, architecture;
    std::map<std::string,std::string> files;
    std::map<std::string,std::string> entrypoints{};
};
GenerationSpec generationSpecFromJson(const nlohmann::json& value);
nlohmann::json generationSpecToJson(const GenerationSpec& spec);
struct Generation {
    std::string id;
    std::filesystem::path directory;
    GenerationSpec spec;
    bool pending=false;
};
class VersionStore {
public:
    using Hash=std::function<std::string(const std::filesystem::path&)>;
    using Checkpoint=std::function<void(const std::string&)>;
    VersionStore(std::filesystem::path root,uid_t owner,Hash hash,Checkpoint checkpoint={},bool create=true);
    ~VersionStore();
    VersionStore(const VersionStore&)=delete;
    VersionStore& operator=(const VersionStore&)=delete;
    std::string stage(const std::filesystem::path& source,const GenerationSpec& spec);
    void seed(const std::string& id);
    void activate(const std::string& id);
    std::optional<Generation> current();
    std::optional<Generation> boot();
    void confirm(const std::string& id);
    void prune();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
