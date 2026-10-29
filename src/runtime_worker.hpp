#pragma once
#include "update_worker.hpp"

namespace pmu {
nlohmann::json handleRuntimeTask(const nlohmann::json& task,const std::string& publisherKey,
                                 const UpdateProgress& progress={},const std::string& runningVersion={},
                                 const std::function<void(const std::string&)>& stoppedCheck={});
}
