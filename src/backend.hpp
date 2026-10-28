#pragma once
#include <utility>
#include <nlohmann/json.hpp>
#include "platform.hpp"
#include "plugin_storage.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
namespace fs = std::filesystem;
using Json = nlohmann::json;
// Where a plugin is released. With an empty tagPrefix the release is the repository's latest one (poloq's layout: one
// plugin per repository). With a tagPrefix the release is the tag tagPrefix + <catalog version>, for a repository that
// publishes other releases too, and assetPrefix (when set) is the start of the package's exact file name.
struct ReleaseSource {
    std::string repo, tagPrefix, assetPrefix;
    ReleaseSource(std::string repository, std::string tags = {}, std::string assets = {}) : repo(std::move(repository)), tagPrefix(std::move(tags)), assetPrefix(std::move(assets)) {}
    ReleaseSource(const char* repository) : repo(repository) {}
};
struct Manager {
    fs::path usb, session, synths, settings;
    std::vector<Json> catalog;
    std::string catalogError;
    const std::string arch;
    Manager(fs::path u, fs::path s, fs::path target, fs::path set, std::string architecture=managerArchitecture(), PluginStoragePaths storagePaths={});
    Json pluginStorageInfo();
    Json pluginStorageFolders(const fs::path& directory);
    void selectPluginStorage(const fs::path& target);
    void openCatalog();
    Json checkManagerUpdate();
    Json stageManagerUpdate(const Json& checkedRelease, const std::function<void(const std::string&)>& progress = {}, const std::function<void(float)>& amount = {});
    void verifyLatestPlugin(const std::string& id, const ReleaseSource& source);
    Json installationOffer(const Json& item);
    void installLatestPlugin(const std::string& id, const ReleaseSource& source, const std::function<void(const std::string&)>& progress = {}, const std::function<void(float)>& amount = {});
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
    PluginStorage pluginStorage;
    bool nativePluginStorage=false,automaticPluginStorage=true;
    std::string pluginStorageIdentity,pluginStorageError;
    void installPackage(const Json& item, const std::function<void(const std::string&)>& progress, const std::function<void(float)>& amount, bool latestOnly);
};
int runProcess(const std::vector<std::string>& args, const std::function<void()>& tick = {}, uint64_t maxFileBytes = 512ULL*1024*1024);
std::string sha256(const fs::path& file);
