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
    std::string authorization{};
};
class VersionStore {
public:
    using Hash=std::function<std::string(const std::filesystem::path&)>;
    using Checkpoint=std::function<void(const std::string&)>;
    using Trust=std::function<void(const std::string&,const GenerationSpec&,const std::string&)>;
    VersionStore(std::filesystem::path root,uid_t owner,Hash hash,Checkpoint checkpoint={},bool create=true,Trust trust={},std::string profile={});
    ~VersionStore();
    VersionStore(const VersionStore&)=delete;
    VersionStore& operator=(const VersionStore&)=delete;
    std::string stage(const std::filesystem::path& source,const GenerationSpec& spec,const std::string& authorization={});
    void seed(const std::string& id);
    void activate(const std::string& id,const std::string& expectedCurrent={});
    bool offer(const std::string& id);
    std::optional<Generation> current();
    Generation inspect(const std::string& id);
    std::optional<Generation> boot();
    std::filesystem::path entrypoint(const std::string& id,const std::string& role);
    void confirm(const std::string& id);
    void reject(const std::string& id);
    void restore(const std::string& knownWorking,const std::string& expectedCurrent);
    void prune();
    unsigned erase();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
