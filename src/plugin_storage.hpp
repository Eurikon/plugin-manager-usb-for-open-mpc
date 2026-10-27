#pragma once
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <sys/types.h>
#include <vector>

struct PluginStoragePaths {
    std::filesystem::path mounts="/proc/self/mountinfo";
    std::function<dev_t(const std::filesystem::path&)> device;
};

class PluginStorage {
public:
    PluginStorage(std::filesystem::path settings,PluginStoragePaths paths={});
    nlohmann::json automatic() const;
    nlohmann::json inspect(const std::filesystem::path& target) const;
    nlohmann::json folders(const std::filesystem::path& directory) const;
    std::vector<nlohmann::json> targets() const;
    void prepare(const std::filesystem::path& target,const std::string& identity) const;
private:
    std::filesystem::path settings;
    PluginStoragePaths paths;
};
