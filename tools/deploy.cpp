#include "backend.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string id, skin, version, uid, name, manufacturer, so, kind;
std::string previousName, previousSkin, previousManufacturer;
const fs::path target = "/sdcard/Expansions";
fs::path library;
volatile std::sig_atomic_t interrupted = 0;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void cancel(int) {
    interrupted = 1;
}
void active() {
    require(interrupted == 0, "Deployment cancelled");
}
bool present(const fs::path &path) {
    return fs::exists(path) || fs::is_symlink(path);
}
bool within(const fs::path &root, const fs::path &path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::string contents(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read deployment metadata");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string installedContents(const fs::path &source, const fs::path &relative) {
    auto result = contents(source);
    if (relative.begin()->string() == "Presets" && relative.extension() == ".xpl") {
        const std::string marker = "%payload-path%";
        size_t at = 0;
        while ((at = result.find(marker, at)) != std::string::npos) {
            result.replace(at, marker.size(), target.string());
            at += target.string().size();
        }
    }
    return result;
}
void unchangedPayload(const fs::path &expected, const fs::path &installed) {
    require(fs::is_directory(installed) && !fs::is_symlink(installed), "Previous plugin payload is unavailable");
    std::set<fs::path> known;
    for (const auto &entry : fs::recursive_directory_iterator(expected)) {
        const auto relative = entry.path().lexically_relative(expected);
        const auto file = installed / relative;
        require(!fs::is_symlink(entry.path()) && !fs::is_symlink(file), "Previous plugin payload contains a symlink");
        const auto expectedContents = fs::is_regular_file(entry.path()) ? installedContents(entry.path(), relative) : "";
        require((fs::is_directory(entry.path()) && fs::is_directory(file)) ||
                    (fs::is_regular_file(entry.path()) && fs::is_regular_file(file) &&
                        fs::file_size(file) == expectedContents.size() && contents(file) == expectedContents),
                "Previous plugin payload changed; preserve user content before migration");
        known.insert(relative);
    }
    for (const auto &entry : fs::recursive_directory_iterator(installed))
        require(known.count(entry.path().lexically_relative(installed)) != 0 && !fs::is_symlink(entry.path()),
                "Previous plugin payload contains unrecognized user content");
}
bool identity(const Json &item, bool previous = false) {
    return item.value("id", "") == id && item.value("uid", "") == uid &&
           item.value("skin", "") == (previous ? previousSkin : skin) &&
           item.value("name", "") == (previous ? previousName : name) &&
           item.value("manufacturer", "") == (previous ? previousManufacturer : manufacturer) &&
           item.value("kind", "") == kind &&
           item.value("so", "") == so;
}
void verifyRegistry(const fs::path &registry) {
    require(fs::is_directory(registry) && !fs::is_symlink(registry), "Managed registry must be an ordinary directory");
    std::set<std::string> expected{"mpc-plugin.json", "uninstall.sh", "plugin_list.awk"};
    for (const auto &entry : fs::directory_iterator(registry))
        require(fs::symlink_status(entry.path()).type() == fs::file_type::regular &&
                    expected.erase(entry.path().filename().string()) == 1,
                "Managed registry contains unrecognized files, directories or symlinks");
    require(expected.empty(), "Managed registry is incomplete");
}
void stopped() {
    const auto status = runProcess({"pidof", "MPC"});
    require(status == 1, status == 0 ? "MPC must be stopped before deployment" : "Cannot query MPC process");
}
void copyTree(const fs::path &source, const fs::path &destination) {
    fs::create_directories(destination.parent_path());
    fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    const auto metadata = [](const fs::path &from, const fs::path &to) {
        if (!fs::is_symlink(from)) {
            fs::permissions(to, fs::status(from).permissions());
            fs::last_write_time(to, fs::last_write_time(from));
        }
    };
    if (fs::is_directory(source))
        for (const auto &entry : fs::recursive_directory_iterator(source))
            metadata(entry.path(), destination / entry.path().lexically_relative(source));
    metadata(source, destination);
}
struct Snapshot {
    fs::path original, saved;
    bool existed;
    Snapshot(fs::path path, fs::path backup) : original(std::move(path)), saved(std::move(backup)) {
        existed = present(original);
        require(!fs::is_symlink(original), "Deployment target cannot be a symlink");
        if (existed)
            copyTree(original, saved);
    }
    void restore() const {
        require(!existed || present(saved), "Rollback snapshot unavailable; current target preserved");
        fs::remove_all(original);
        if (existed)
            copyTree(saved, original);
    }
};
std::set<fs::path> backups(const fs::path &settings) {
    std::set<fs::path> result;
    const auto prefix = settings.filename().string() + ".bak-" + fs::path(so).stem().string() + "-";
    for (const auto &entry : fs::directory_iterator(settings.parent_path()))
        if (entry.path().filename().string().rfind(prefix, 0) == 0)
            result.insert(entry.path());
    return result;
}
void safeBackups(const fs::path &settings, const std::set<fs::path> &existing) {
    const auto now = std::time(nullptr);
    for (int offset = 0; offset < 600; ++offset) {
        const auto instant = now + offset;
        std::tm time{};
        require(localtime_r(&instant, &time) != nullptr, "Cannot check settings backup timestamps");
        std::array<char, 32> stamp{};
        for (const char* format : {"%d-%m-%Y-%H%M%S", "%Y%m%d-%H%M%S"}) {
            require(std::strftime(stamp.data(), stamp.size(), format, &time) != 0,
                    "Cannot format settings backup timestamp");
            require(existing.count(fs::path(settings.string() + ".bak-" + fs::path(so).stem().string() + "-" + stamp.data())) == 0,
                    "A preexisting settings backup would be overwritten");
        }
    }
}
void cleanBackups(const fs::path &settings, const std::set<fs::path> &existing) {
    for (const auto &file : backups(settings))
        if (existing.count(file) == 0)
            fs::remove(file);
}
std::vector<std::string> otherPlugins(const std::string &xml, const fs::path &expectedLibrary,
                                     const std::string &expectedName, const std::string &expectedManufacturer) {
    std::vector<std::string> result;
    const std::regex tag("<PLUGIN\\s+[^>]*>"), attribute("([A-Za-z][A-Za-z0-9_-]*)\\s*=\\s*\"([^\"]*)\"");
    for (std::sregex_iterator item(xml.begin(), xml.end(), tag), end; item != end; ++item) {
        const auto text = item->str();
        Json attrs = Json::object();
        for (std::sregex_iterator part(text.begin(), text.end(), attribute); part != end; ++part)
            attrs[(*part)[1].str()] = (*part)[2].str();
        if (attrs.value("uid", "") == uid || attrs.value("file", "") == library.string() ||
                attrs.value("file", "") == expectedLibrary.string()) {
            require(attrs.value("uid", "") == uid && attrs.value("file", "") == expectedLibrary.string() &&
                        attrs.value("name", "") == expectedName &&
                        attrs.value("manufacturer", "") == expectedManufacturer,
                    "Plugin settings identity collides with another installation");
        } else {
            result.push_back(text);
        }
    }
    return result;
}
std::string otherSettings(const std::string &xml) {
    auto text =
        std::regex_replace(xml, std::regex("<VALUE\\s+name=\"pluginList-arm\"[^>]*>[\\s\\S]*?</VALUE>"), "");
    text.erase(
        std::remove_if(text.begin(), text.end(), [](unsigned char value) { return std::isspace(value); }),
        text.end());
    return text;
}
struct Effect;
using Host = intptr_t (*)(Effect*, int32_t, int32_t, intptr_t, void*, float);
struct Effect {
    int32_t magic;
    intptr_t (*dispatcher)(Effect*, int32_t, int32_t, intptr_t, void*, float);
    void (*process)(Effect*, float**, float**, int32_t);
    void (*setParameter)(Effect*, int32_t, float);
    float (*getParameter)(Effect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t reserved1, reserved2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
};
void verifyLibrary(const fs::path& path, const Json& manifest) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 20> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    require(bool(input) && header[0] == 127 && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
                header[4] == 1 && header[5] == 1 && header[18] == 40 && header[19] == 0,
            "The selected library is not a 32-bit ARM plugin");
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(module != nullptr, "Cannot resolve the selected plugin's firmware libraries");
    const auto entry = reinterpret_cast<Effect* (*)(Host)>(dlsym(module, "VSTPluginMain"));
    Effect* effect = entry ? entry(nullptr) : nullptr;
    const bool valid = effect && effect->magic == 0x56737450 &&
        uint32_t(effect->uniqueID) == std::stoul(uid, nullptr, 16) && effect->dispatcher &&
        effect->numParams >= 0 && effect->numParams <= 4096 && effect->numOutputs == 2 &&
        effect->numInputs == (kind == "instrument" ? 0 : 2) &&
        ((effect->flags & (1 << 8)) != 0) == (kind == "instrument") &&
        (!manifest.contains("vst_version") || effect->version == manifest.at("vst_version").get<int>());
    if (effect && effect->dispatcher) effect->dispatcher(effect, 1, 0, 0, nullptr, 0);
    dlclose(module);
    require(valid, "The selected VST identity differs from the package");
}
}

int main(int argc, char **argv) {
    fs::path transaction;
    std::vector<Snapshot> snapshots;
    std::vector<fs::path> temporary;
    std::set<fs::path> existingBackups;
    fs::path settings, registryParent;
    bool changed = false, ownedRegistryParent = false;
    try {
        require(argc == 8 || argc == 10,
                "Usage: mpc-plugin-deploy USB SESSION SETTINGS ZIP_FILENAME ID VERSION SHA256 [PREVIOUS_ZIP PREVIOUS_SHA256]");
        id = argv[5]; version = argv[6];
        require(std::regex_match(id, std::regex("[a-z0-9][a-z0-9_-]*")) &&
                    std::regex_match(version, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")) &&
                    std::regex_match(argv[7], std::regex("[0-9a-f]{64}")), "Invalid package selection");
        require(std::signal(SIGTERM, cancel) != SIG_ERR && std::signal(SIGINT, cancel) != SIG_ERR,
                "Cannot register deployment cancellation");
        const fs::path usb = argv[1], session = argv[2], filename = argv[4];
        settings = argv[3];
        require(usb.is_absolute() && session.is_absolute() && settings.is_absolute(),
                "Deployment paths must be absolute");
        require(fs::is_directory(usb / "local-plugins") && fs::is_directory(session) &&
                    fs::is_regular_file(settings) && !fs::is_symlink(settings),
                "USB, session or settings are unavailable");
        require(within("/tmp", fs::canonical(session)) || within("/run", fs::canonical(session)),
                "The deployment session must be in RAM");
        require(!filename.empty() && filename == filename.filename() && filename.extension() == ".zip" &&
                    filename.string().front() != '.',
                "Select one local ZIP filename");
        const auto zip = usb / "local-plugins" / filename;
        require(fs::is_regular_file(zip) && !fs::is_symlink(zip) &&
                    within(fs::canonical(usb / "local-plugins"), fs::canonical(zip)),
                "Selected local ZIP is unavailable or outside the USB folder");
        stopped();
        umask(0077);
        std::string pattern = (session / "mpc-plugin-deploy-XXXXXX").string();
        const auto created = mkdtemp(pattern.data());
        require(created != nullptr, "Cannot create a private deployment transaction");
        transaction = created;
        Manager manager(usb, transaction / "manager", target, settings);
        manager.validateTarget();
        require(!fs::is_symlink(target), "A symlink SD target is not supported");
        const auto registry = target / ".mpc-usb-manager" / id;
        registryParent = registry.parent_path();
        require(!fs::is_symlink(registryParent) &&
                    (!present(registryParent) || fs::is_directory(registryParent)),
                "The managed SD registry cannot be a symlink or file");
        ownedRegistryParent = !present(registryParent);
        if (present(registry))
            verifyRegistry(registry);
        require(!std::getenv("MPC_LEGACY_ROOT") || !*std::getenv("MPC_LEGACY_ROOT"),
                "A legacy installation override is not supported");
        const auto hash = sha256(zip);
        require(hash == argv[7], "Selected USB ZIP differs from the locally verified archive");
        Json selected;
        for (const auto &item : manager.list("local"))
            if (item.value("path", "") == zip.string()) {
                require(selected.is_null(), "The selected local ZIP is ambiguous");
                selected = item;
            }
        require(!selected.is_null() && selected.value("sha", "") == hash && selected.value("id", "") == id &&
                    selected.value("version", "") == version,
                "The selected ZIP identity, version or checksum differs");
        const auto package = manager.unpack(zip, transaction / "preflight");
        const auto manifest = manager.manifest(package);
        skin = manifest.at("skin").get<std::string>(); uid = manifest.at("uid").get<std::string>();
        name = manifest.at("name").get<std::string>(); manufacturer = manifest.at("manufacturer").get<std::string>();
        so = manifest.at("so").get<std::string>(); kind = manifest.at("kind").get<std::string>();
        require(std::regex_match(uid, std::regex("[0-9a-f]{8}")) &&
                    (kind == "instrument" || kind == "effect") &&
                    std::regex_match(so, std::regex("[A-Za-z0-9_.-]+\\.so")) &&
                    !skin.empty() && skin.front() != '.', "Invalid native plugin identity");
        library = target / skin / so;
        require(!present(fs::path("/sdcard/vst") / so), "Legacy installation needs separate handling");
        require(identity(manifest) && manifest.at("version") == version &&
                    manifest.value("layout", "") == "portable" &&
                    manifest.at("folder") == "portable/" + skin && manifest.at("user_data") == Json::array(),
                "Package identity differs or declares unsupported in-payload user data");
        Json previousManifest;
        fs::path previousPackage, previousPayload;
        previousName = name;
        previousSkin = skin;
        previousManufacturer = manufacturer;
        if (argc == 10) {
            const fs::path previousZip = argv[8];
            require(previousZip.is_absolute() && fs::is_regular_file(previousZip) && !fs::is_symlink(previousZip) &&
                        fs::file_size(previousZip) <= 32 * 1024 * 1024 &&
                        (within("/tmp", fs::canonical(previousZip)) || within("/run", fs::canonical(previousZip))) &&
                        std::regex_match(argv[9], std::regex("[0-9a-f]{64}")) && sha256(previousZip) == argv[9],
                    "Previous package must be an exact verified RAM archive");
            previousPackage = manager.unpack(previousZip, transaction / "previous-package");
            previousManifest = manager.manifest(previousPackage);
            previousName = previousManifest.at("name").get<std::string>();
            previousSkin = previousManifest.at("skin").get<std::string>();
            previousManufacturer = previousManifest.at("manufacturer").get<std::string>();
            require(identity(previousManifest, true) && previousManifest.value("layout", "") == "portable" &&
                        previousManifest.at("folder") == "portable/" + previousSkin &&
                        previousManifest.at("user_data") == Json::array() &&
                        previousManifest.value("param_compat", 0) == manifest.value("param_compat", 0) &&
                        std::regex_match(previousManifest.at("version").get<std::string>(),
                                         std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")),
                    "Previous package changes the stable plugin identity or declares user data");
            previousPayload = previousPackage / previousManifest.at("folder").get<std::string>();
            require(Json::parse(contents(previousPayload / "mpc-plugin.json")) == previousManifest,
                    "Previous package payload metadata differs");
            require(sha256(previousZip) == argv[9], "Previous archive changed during preflight");
        }
        const auto safeText = [](const std::string &value) {
            return !value.empty() && value.size() <= 256 &&
                std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
        };
        require(safeText(previousName) && safeText(previousSkin) && safeText(previousManufacturer) &&
                    previousSkin.front() != '.' &&
                    previousSkin.find('/') == std::string::npos && previousSkin.find('\\') == std::string::npos,
                "Invalid previous plugin identity");
        for (const auto &field : manifest.items())
            require(selected.contains(field.key()) && selected.at(field.key()) == field.value(),
                    "Selected local metadata differs from the complete package manifest");
        const auto payload = package / manifest.at("folder").get<std::string>();
        require(Json::parse(contents(payload / "mpc-plugin.json")) == manifest &&
                    fs::is_regular_file(payload / "Plugin Skins/TUI.json"),
                "Package payload metadata or required assets are incomplete");
        verifyLibrary(payload / so, manifest);
        require(sha256(zip) == hash, "The selected ZIP changed during preflight");
        Json previous;
        bool renamed = false;
        for (const auto &item : manager.installedPlugins())
            if (item.value("id", "") == id || item.value("uid", "") == uid ||
                item.value("skin", "") == skin || item.value("skin", "") == previousSkin) {
                require(previous.is_null() && (identity(item) || identity(item, true)) && item.value("managed", false) &&
                            item.value("device_target", "") == target.string() &&
                            item.value("device_file", "") == (target / item.at("skin").get<std::string>() / so).string() &&
                            std::regex_match(item.value("version", ""), std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")),
                        "Existing plugin must be one matching managed SD installation");
                previous = item;
                renamed = !identity(item);
            }
        require(!previous.is_null() || (!present(target / skin) && !present(registry)),
                "An unrecognized Plugin payload or registry would be overwritten");
        require(!renamed || !present(target / skin), "The renamed plugin destination is already occupied");
        if (!previous.is_null()) {
            const auto record = Json::parse(contents(registry / "mpc-plugin.json"));
            require(identity(record, renamed) && record.at("version") == previous.at("version") &&
                        record.at("user_data") == Json::array() &&
                        Json::parse(contents(target / record.at("skin").get<std::string>() / "mpc-plugin.json")) == record &&
                        fs::is_regular_file(registry / "plugin_list.awk") &&
                        contents(registry / "uninstall.sh").find("DEFER=") != std::string::npos,
                    "Existing uninstall record differs from the verified Plugin installer");
            if (renamed) {
                require(!previousManifest.is_null() && record == previousManifest,
                        "Existing plugin differs from the explicitly verified previous package");
                for (const auto &file : {"uninstall.sh", "plugin_list.awk"})
                    require(contents(registry / file) == contents(previousPackage / file),
                            "Previous uninstall record changed; migration refused");
                unchangedPayload(previousPayload, target / previousSkin);
            } else if (argc == 10) {
                unchangedPayload(payload, target / skin);
            }
        }
        const auto originalSettings = contents(settings);
        const auto plugins = otherPlugins(originalSettings, renamed ? target / previousSkin / so : library,
                                           renamed ? previousName : name,
                                           renamed ? previousManufacturer : manufacturer);
        const auto history = usb / "plugin-history.json";
        Json previousHistory = {{"schema", 1}, {"plugins", Json::object()}};
        if (present(history)) {
            previousHistory = Json::parse(contents(history));
            require(previousHistory.value("schema", 0) == 1 && previousHistory.at("plugins").is_object(),
                    "USB installation history is invalid");
        }
        temporary = {fs::path(settings.string() + ".new"), fs::path(settings.string() + ".entry"),
                     fs::path(history.string() + ".tmp"),  target / ("." + skin + ".new"),
                     target / ("." + skin + ".old"),       target / ("." + skin + ".keep")};
        if (renamed)
            for (const auto &suffix : {".new", ".old", ".keep"})
                temporary.push_back(target / ("." + previousSkin + suffix));
        for (const auto &path : temporary)
            require(!present(path), "A preexisting installer temporary must be preserved");
        existingBackups = backups(settings);
        safeBackups(settings, existingBackups);
        std::vector<fs::path> paths{target / skin, registry, settings, history};
        if (renamed)
            paths.push_back(target / previousSkin);
        for (size_t index = 0; index < paths.size(); ++index)
            snapshots.emplace_back(paths[index], transaction / ("snapshot-" + std::to_string(index)));
        stopped();
        active();
        changed = true;
        if (!previous.is_null()) {
            manager.remove(previous);
            require(!present(target / previous.at("skin").get<std::string>()) && !present(registry),
                    "Existing Plugin was not fully removed");
        }
        active();
        safeBackups(settings, existingBackups);
        manager.install(selected, [](const std::string &) { active(); });
        active();
        fs::permissions(settings, fs::status(snapshots[2].saved).permissions());
        if (snapshots[3].existed)
            fs::permissions(history, fs::status(snapshots[3].saved).permissions());
        Json installed;
        for (const auto &item : manager.installedPlugins())
            if (item.value("id", "") == id) {
                require(installed.is_null(), "More than one Plugin installation remains");
                installed = item;
            }
        require(!installed.is_null() && identity(installed) && installed.value("version", "") == version &&
                    installed.value("managed", false) &&
                    installed.value("device_file", "") == library.string(),
                "Installed Plugin identity or version failed verification");
        require(Json::parse(contents(target / skin / "mpc-plugin.json")) == manifest &&
                    Json::parse(contents(registry / "mpc-plugin.json")) == manifest,
                "Installed payload or manager metadata differs from the package");
        for (const auto& file : fs::recursive_directory_iterator(payload)) {
            if (!fs::is_regular_file(file.path())) continue;
            const auto relative = file.path().lexically_relative(payload);
            const auto installedFile = target / skin / relative;
            const auto expected = installedContents(file.path(), relative);
            require(fs::is_regular_file(installedFile) && contents(installedFile) == expected,
                    "Installed plugin asset checksum failed verification");
        }
        for (const auto &file : {"uninstall.sh", "plugin_list.awk"})
            require(sha256(registry / file) == sha256(package / file),
                    "Manager uninstall record was not installed");
        const auto updatedSettings = contents(settings);
        require(otherPlugins(updatedSettings, library, name, manufacturer) == plugins &&
                    otherSettings(updatedSettings) == otherSettings(originalSettings),
                "Other plugin registrations or MPC settings changed");
        auto updatedHistory = Json::parse(contents(history));
        const auto saved = updatedHistory.at("plugins").at(id);
        require(saved.value("installed", false) && saved.value("version", "") == version &&
                    saved.value("id", "") == id && saved.value("skin", "") == skin &&
                    saved.value("name", "") == name && saved.value("kind", "") == kind &&
                    saved.value("manufacturer", "") == manufacturer,
                "USB installation history does not confirm the new build");
        previousHistory["plugins"].erase(id);
        updatedHistory["plugins"].erase(id);
        require(updatedHistory == previousHistory, "Other USB installation history changed");
        for (const auto &path : temporary)
            require(!present(path), "Installer left a transaction temporary behind");
        active();
        changed = false;
        bool cleaned = true;
        try {
            cleanBackups(settings, existingBackups);
            fs::remove_all(transaction);
            transaction.clear();
        } catch (...) {
            cleaned = false;
            std::cerr << "Plugin committed; cleanup incomplete. Remaining snapshots: "
                      << transaction.string() << '\n';
        }
        std::cout << Json{{"id", id},
                          {"version", version},
                          {"installed", true},
                          {"replaced_version", previous.is_null() ? Json(nullptr) : previous.at("version")},
                          {"replaced_identity", previous.is_null() ? Json(nullptr) :
                              Json{{"name", previous.at("name")}, {"skin", previous.at("skin")},
                                   {"manufacturer", previous.at("manufacturer")}}},
                          {"previous_package_sha256", argc == 10 ? Json(argv[9]) : Json(nullptr)},
                          {"target", (target / skin).string()},
                          {"sha256", hash},
                          {"cleanup_complete", cleaned},
                          {"snapshot_directory", cleaned ? Json(nullptr) : Json(transaction.string())}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        bool restored = true;
        if (changed) {
            for (auto item = snapshots.rbegin(); item != snapshots.rend(); ++item)
                try {
                    item->restore();
                } catch (...) {
                    restored = false;
                }
            try {
                for (const auto &path : temporary)
                    fs::remove_all(path);
                cleanBackups(settings, existingBackups);
                if (ownedRegistryParent && fs::is_directory(registryParent) && fs::is_empty(registryParent))
                    fs::remove(registryParent);
            } catch (...) {
                restored = false;
            }
        }
        if (restored && !transaction.empty()) {
            std::error_code ignored;
            fs::remove_all(transaction, ignored);
        }
        std::cerr << "Plugin deployment failed: " << error.what() << '\n';
        if (changed)
            std::cerr << (restored ? "Previous installation restored.\n"
                                   : "Rollback incomplete; retain snapshots.\n");
        return 1;
    }
}
