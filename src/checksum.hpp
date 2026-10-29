#pragma once
#include <chrono>
#include <filesystem>
#include <string>

namespace pmu {
std::string fileSha256(const std::filesystem::path& path,std::chrono::milliseconds timeout=std::chrono::seconds(30));
}
