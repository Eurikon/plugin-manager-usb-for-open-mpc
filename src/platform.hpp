#pragma once

#if defined(__aarch64__)
#define PMU_HELPER_DIRECTORY "/nvme/MPC Documents/Devices/Plugin Manager helper"
#define PMU_FIRMWARE_SHA256 "b80687ed4f6b5233486a9e20ff043b35e9863bc6460bd573a249421133d77b63"
#else
#define PMU_HELPER_DIRECTORY "/sdcard/MPC Documents/Devices/Plugin Manager helper"
#define PMU_FIRMWARE_SHA256 "753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494"
#endif

#ifdef __cplusplus
inline const char* managerArchitecture() {
#if defined(__aarch64__)
    return "aarch64";
#else
    return "armv7";
#endif
}
#endif

#ifdef __cplusplus
#include <string>
inline std::string managerRuntimePath(const std::string& name, const std::string& arch) {
    return arch == "aarch64" && (name == "manager" || name == "shortcut" || name == "shortcut-hook.so")
        ? "aarch64/" + name : name;
}
inline const char* managerRuntimeManifest(const std::string& arch) {
    return arch == "aarch64" ? "runtime-aarch64.sha256" : "runtime.sha256";
}
#endif
