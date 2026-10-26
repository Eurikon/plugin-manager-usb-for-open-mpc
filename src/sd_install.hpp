#pragma once
#include "backend.hpp"
#include <sys/types.h>

struct SdInstallPaths {
    fs::path media="/media", mountinfo="/proc/self/mountinfo", uuids="/dev/disk/by-uuid";
    fs::path sys="/sys", udev="/run/udev/data";
    fs::path internal="/sdcard", helper="/sdcard/MPC Documents/Devices/Plugin Manager helper";
    fs::path firmware="/usr/bin/MPC";
    std::vector<fs::path> protectedRoots={"/","/data","/sdcard","/media/az01-internal"};
    std::string firmwareHash="753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494";
    uid_t owner=0;
    std::function<dev_t(const fs::path&)> fileDevice, blockDevice;
    std::function<uint64_t(const fs::path&)> availableBytes;
};

class SdInstaller {
public:
    SdInstaller(fs::path source, fs::path session);
    SdInstaller(fs::path source, fs::path session, SdInstallPaths paths);
    bool available() const noexcept;
    Json inspect();
    std::vector<Json> targets();
    void install(const std::string& uuid, const std::function<void(const std::string&)>& progress={},
                 const std::function<void(float)>& amount={});
    void remove(const std::function<void(const std::string&)>& progress={},
                const std::function<void(float)>& amount={});
private:
    fs::path source, session;
    SdInstallPaths paths;
    bool enabled=true;
};
