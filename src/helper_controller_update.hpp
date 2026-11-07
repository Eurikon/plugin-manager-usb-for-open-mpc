#pragma once
#include "update_bundle.hpp"
#include <optional>

namespace pmu {
bool stageControllerUpdate(const UpdateBundle& bundle,const std::filesystem::path& helper,
                           const std::filesystem::path& recovery,const std::string& publisher,
                           const VersionStore::Checkpoint& checkpoint={});
void applyControllerUpdate(const std::filesystem::path& recovery,const VersionStore::Checkpoint& checkpoint={});
void rollbackControllerUpdate(const std::filesystem::path& recovery);
std::optional<NativeImageSpec> controllerRecovery(const std::filesystem::path& recovery);
int recoverControllerHandoff(const std::filesystem::path& capsule);
}
