#pragma once
#include "native_image.hpp"
#include "version_store.hpp"

namespace pmu {
struct RuntimeChoice {
    Generation generation;
    NativeImageSpec worker;
    std::string identity;
};
class SourceRuntime {
public:
    SourceRuntime(std::filesystem::path source,std::string architecture,std::string publisherKey);
    ~SourceRuntime();
    SourceRuntime(const SourceRuntime&)=delete;
    SourceRuntime& operator=(const SourceRuntime&)=delete;
    RuntimeChoice choose(bool opening,const std::string& expectedIdentity={});
    RuntimeChoice retained(const std::string& generation,const std::string& expectedIdentity);
    void confirm(const std::string& generation,const std::string& expectedIdentity);
    RuntimeChoice recover(const std::string& failedGeneration,const std::string& expectedIdentity);
    std::string identity()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
