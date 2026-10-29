#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "native_image.hpp"

namespace pmu {
struct SessionHandoff {
    std::filesystem::path source,helper,oldBase,newBase,rollbackBase;
    std::string generation,previousGeneration,helperGeneration,previousHelperGeneration,sourceIdentity,service;
    NativeImageSpec rollbackWorker;
    bool openAfterHandoff=false;
};
class UpdateSession {
public:
    UpdateSession(std::filesystem::path source,std::filesystem::path cache,
                  std::string generation,std::string sourceIdentity);
    ~UpdateSession();
    UpdateSession(const UpdateSession&)=delete;
    UpdateSession& operator=(const UpdateSession&)=delete;
    std::string preloadSnapshot()const;
    std::string mpcService()const;
    std::filesystem::path publish(const std::filesystem::path& prepared,
                                 const std::filesystem::path& helper,
                                 const std::string& generation,const std::filesystem::path& rollbackBase,
                                 const NativeImageSpec& rollbackWorker,const std::string& helperGeneration={},
                                 const std::string& previousHelperGeneration={},bool openAfterHandoff=false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
SessionHandoff readSessionHandoff(const std::filesystem::path& path);
std::filesystem::path queuedSessionHandoff(const std::filesystem::path& cache);
}
