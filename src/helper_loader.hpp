#pragma once
#include <filesystem>
#include <memory>
#include <string>

namespace pmu {
class HelperLoader {
public:
    HelperLoader(std::filesystem::path store,std::filesystem::path helperDirectory,std::string architecture);
    ~HelperLoader();
    HelperLoader(const HelperLoader&)=delete;
    HelperLoader& operator=(const HelperLoader&)=delete;
    bool start(void* binding);
    void stop();
    bool healthy()const;
    std::string generation()const;
    std::string error()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
