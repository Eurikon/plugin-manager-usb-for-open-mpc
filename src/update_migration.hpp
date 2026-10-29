#pragma once
#include "update_worker.hpp"

namespace pmu {
struct MigrationPayload {
    std::filesystem::path directory;
    GenerationSpec specification;
    std::string authorization;
};
struct MigrationSelection {
    std::string runtime,helper;
};
MigrationSelection activateMigration(const std::filesystem::path& source,
    const std::filesystem::path& helper,const std::string& sourceIdentity,
    const std::string& architecture,const std::string& publisherKey,
    const MigrationPayload& runtime,const std::optional<MigrationPayload>& helperPayload,
    const UpdateProgress& progress={},const VersionStore::Checkpoint& checkpoint={});
}
