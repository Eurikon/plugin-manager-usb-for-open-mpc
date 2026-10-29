#pragma once
#include <filesystem>
#include <string>

namespace pmu {
void beginRuntimeHandoff(const std::filesystem::path& capsule,const std::string& publisher);
void finalizeRuntimeHandoff(const std::filesystem::path& capsule,const std::string& publisher);
void recoverRuntimeHandoff(const std::filesystem::path& capsule,const std::string& publisher);
}
