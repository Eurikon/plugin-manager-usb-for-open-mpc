#pragma once
#include "backend.hpp"
#include <sys/types.h>
#include <publisher_key.hpp>

struct SdInstallPaths {
    fs::path media="/media", mountinfo="/proc/self/mountinfo", uuids="/dev/disk/by-uuid";
    fs::path sys="/sys", udev="/run/udev/data";
    fs::path internal=std::string(managerArchitecture())=="aarch64"?"/nvme":"/sdcard", helper=PMU_HELPER_DIRECTORY;
    fs::path firmware="/usr/bin/MPC";
    std::vector<fs::path> protectedRoots={"/","/data","/sdcard","/nvme","/storage","/media/az01-internal"};
    std::string firmwareHash=PMU_FIRMWARE_SHA256;
    std::string arch=managerArchitecture();
    std::string publisher=pmu::publisherKey;
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
