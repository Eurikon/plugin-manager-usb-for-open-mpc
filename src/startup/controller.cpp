#include "helper_loader.hpp"
#include "platform.hpp"
#include "storage.h"
#include <publisher_key.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits.h>
#include <stdexcept>
#include <unistd.h>

namespace {
pmu::HelperLoader *loader = nullptr;
std::string installation() {
    Dl_info image{};
    char resolved[PATH_MAX];
    if (!dladdr(reinterpret_cast<void *>(&installation), &image) || !image.dli_fname
        || !realpath(image.dli_fname, resolved) || strcmp(image.dli_fname, resolved) != 0)
        throw std::runtime_error("Cannot identify the helper controller");
    char *leaf = strrchr(resolved, '/');
#if defined(__aarch64__)
    constexpr const char *name = "/manager-entry-aarch64.so";
#else
    constexpr const char *name = "/manager-entry.so";
#endif
    if (!leaf || strcmp(leaf, name) != 0) throw std::runtime_error("Invalid helper controller name");
    *leaf = 0;
#if defined(__aarch64__)
    if (!pmu_helper_directory_valid(resolved)) throw std::runtime_error("Unsupported helper controller location");
#else
    if (strcmp(resolved, PMU_HELPER_DIRECTORY) != 0) throw std::runtime_error("Unsupported helper controller location");
#endif
    return resolved;
}
}
extern "C" int pmu_start_manager(void *binding) {
    if (loader || geteuid() != 0) return 0;
    try {
        const auto root = installation();
        loader = new pmu::HelperLoader(root + "/.pmu-versions", root, managerArchitecture(), pmu::publisherKey);
        if (!loader->start(binding)) {
            const auto error = loader->error();
            delete loader;
            loader = nullptr;
            throw std::runtime_error(error);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Plugin manager controller: %.2048s\n", error.what());
    }
    return 0;
}
extern "C" int pmu_stop_manager(void *) {
    delete loader;
    loader = nullptr;
    return 0;
}
