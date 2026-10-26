#pragma once
#include "backend.hpp"
#include <sys/types.h>

struct ManagerUpdatePaths {
    fs::path media="/media", mountinfo="/proc/self/mountinfo", uuids="/dev/disk/by-uuid";
    fs::path helper=PMU_HELPER_DIRECTORY;
    fs::path state="/run/mpc-usb-manager-session", ram="/tmp", firmware="/usr/bin/MPC";
    std::vector<fs::path> protectedRoots={"/","/data","/sdcard","/nvme","/storage","/media/az01-internal"};
    std::string firmwareHash=PMU_FIRMWARE_SHA256;
    std::string arch=managerArchitecture();
    uid_t owner=0;
    std::function<dev_t(const fs::path&)> fileDevice, blockDevice;
    std::function<void()> requireStopped;
    std::function<void(size_t)> beforePublish;
    std::function<void(size_t)> beforeRename;
};

Json applyManagerUpdate(const fs::path& source,const fs::path& session,const Json& staged,
                        const std::function<void(const std::string&)>& progress={},
                        const std::function<void(float)>& amount={});
Json applyManagerUpdate(const fs::path& source,const fs::path& session,const Json& staged,
                        ManagerUpdatePaths paths,
                        const std::function<void(const std::string&)>& progress={},
                        const std::function<void(float)>& amount={});
