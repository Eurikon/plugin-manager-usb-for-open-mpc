#pragma once
#include "version_store.hpp"
#include "native_image.hpp"
#include <chrono>
#include <nlohmann/json.hpp>

namespace pmu {
struct UpdateProgress {
    std::function<void(const std::string&)> message;
    std::function<void(float)> fraction;
};
using IncomingWorker=NativeImageSpec;
using UpdateHandler=std::function<nlohmann::json(const nlohmann::json&,const UpdateProgress&)>;
IncomingWorker generationWorker(const Generation& generation,const std::string& architecture);
nlohmann::json runIncomingWorker(const IncomingWorker& worker,const std::filesystem::path& session,
                                const nlohmann::json& task,const UpdateProgress& progress={},
                                std::chrono::milliseconds timeout=std::chrono::minutes(3));
int serveUpdateWorker(const UpdateHandler& handler);
}
