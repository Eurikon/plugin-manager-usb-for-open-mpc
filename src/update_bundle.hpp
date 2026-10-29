#pragma once
#include "update_worker.hpp"
#include <cstdint>

namespace pmu {
struct UpdateBundle {
    std::filesystem::path directory;
    GenerationSpec spec;
    IncomingWorker worker;
};
nlohmann::json updateEnvelope(const GenerationSpec& spec);
UpdateBundle unpackUpdateBundle(const std::filesystem::path& zip,const std::string& sha256,
                                uint64_t bytes,const std::filesystem::path& destination,
                                const std::string& architecture,const std::string& version,
                                const UpdateProgress& progress={});
}
