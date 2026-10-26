#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Manager {
    fs::path usb, session, synths, settings;
    std::vector<Json> catalog;
    std::string catalogError;
    Manager(fs::path u, fs::path s, fs::path target, fs::path set);
    void openCatalog();
    Json checkManagerUpdate();
    Json stageManagerUpdate(const Json& checkedRelease, const std::function<void(const std::string&)>& progress = {}, const std::function<void(float)>& amount = {});
    void verifyLatestPlugin(const std::string& id, const std::string& repo);
    Json installationOffer(const Json& item);
    void installLatestPlugin(const std::string& id, const std::string& repo, const std::function<void(const std::string&)>& progress = {}, const std::function<void(float)>& amount = {});
    std::vector<Json> list(const std::string& view);
    void install(const Json& item, const std::function<void(const std::string&)>& progress = {}, const std::function<void(float)>& amount = {});
    void remove(const Json& item);
    fs::path unpack(const fs::path& zip, const fs::path& destination);
    Json manifest(const fs::path& package, const Json* catalogItem = nullptr);
    void validateTarget();
    std::vector<Json> installedPlugins();
    void recordInstalled();
    void remember(const Json& item, bool installed);
    bool hideWelcome();
    void setHideWelcome(bool hidden);
    bool darkTheme();
    void setDarkTheme(bool dark);
private:
    void installPackage(const Json& item, const std::function<void(const std::string&)>& progress, const std::function<void(float)>& amount, bool latestOnly);
};
int runProcess(const std::vector<std::string>& args, const std::function<void()>& tick = {}, uint64_t maxFileBytes = 512ULL*1024*1024);
std::string sha256(const fs::path& file);
