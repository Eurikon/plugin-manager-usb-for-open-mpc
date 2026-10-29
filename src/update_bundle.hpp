#pragma once
#include "update_worker.hpp"
#include <cstdint>

namespace pmu {
struct UpdateBundle {
    std::filesystem::path directory;
    GenerationSpec spec;
    IncomingWorker worker;
};
nlohmann::json updateEnvelope(const GenerationSpec& spec,const std::string& authorization={});
UpdateBundle verifyStagedUpdate(const std::filesystem::path& directory,const std::string& architecture,
                                const std::string& version,const std::string& publisherKey);
std::string readUpdateFile(const UpdateBundle& bundle,const std::string& name,size_t maximum=128*1024);
UpdateBundle unpackUpdateBundle(const std::filesystem::path& zip,const std::string& sha256,
                                uint64_t bytes,const std::filesystem::path& destination,
                                const std::string& architecture,const std::string& version,const std::string& publisherKey,
                                const UpdateProgress& progress={});
}
