#pragma once
#include "source_runtime.hpp"
#include "update_worker.hpp"
#include <vector>

namespace pmu {
enum class RuntimeStart { Quiet,StageOnly };
std::filesystem::path prepareRuntimeSession(const RuntimeChoice& selected,
    const std::filesystem::path& source,const std::filesystem::path& helper,
    const std::string& publisherKey,const UpdateProgress& progress={},
    const std::string& preloadSnapshot={},RuntimeStart start=RuntimeStart::Quiet,bool retained=false);
void startPreparedRuntime(const std::filesystem::path& base,const std::string& publisherKey);
NativeImageSpec preparedRuntimeWorker(const std::filesystem::path& base,const std::string& publisherKey,bool retained=false);
nlohmann::json guiRuntimeContext(const std::filesystem::path& work);
int runtimeCommand(const std::vector<std::string>& arguments,const std::filesystem::path& helper={},
                   std::chrono::seconds timeout=std::chrono::seconds(20));
std::pair<int,std::string> runtimeOutput(const std::vector<std::string>& arguments,
                                       std::chrono::seconds timeout=std::chrono::seconds(5));
}
