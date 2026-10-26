#pragma once
#include "backend.hpp"
#include <sys/types.h>

struct ManagerUpdatePaths {
    fs::path media="/media", mountinfo="/proc/self/mountinfo", uuids="/dev/disk/by-uuid";
    fs::path helper="/sdcard/MPC Documents/Devices/Plugin Manager helper";
    fs::path state="/run/mpc-usb-manager-session", ram="/tmp", firmware="/usr/bin/MPC";
    std::vector<fs::path> protectedRoots={"/","/data","/sdcard","/media/az01-internal"};
    std::string firmwareHash="753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494";
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
