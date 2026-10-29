#pragma once
#include "update_bundle.hpp"
#include "update_migration.hpp"

namespace pmu {
struct MigrationPackage {
    MigrationPayload runtime;
    std::optional<MigrationPayload> helper;
};
MigrationPackage readMigrationPackage(const UpdateBundle& bundle,const std::string& architecture);
}
