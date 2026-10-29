#pragma once
#include <filesystem>
#include <string>

namespace pmu {
struct NativeImageSpec {
    std::filesystem::path executable;
    std::string sha256,architecture;
};
class PinnedNativeImage {
    int value=-1;
    explicit PinnedNativeImage(int fd):value(fd){}
    friend PinnedNativeImage pinNativeImage(const NativeImageSpec&,const std::filesystem::path&);
public:
    PinnedNativeImage()=default;
    ~PinnedNativeImage();
    PinnedNativeImage(const PinnedNativeImage&)=delete;
    PinnedNativeImage& operator=(const PinnedNativeImage&)=delete;
    PinnedNativeImage(PinnedNativeImage&& other)noexcept;
    PinnedNativeImage& operator=(PinnedNativeImage&& other)noexcept;
    int descriptor()const{return value;}
};
PinnedNativeImage pinNativeImage(const NativeImageSpec& image,const std::filesystem::path& scratch);
void validateNativeImage(const NativeImageSpec& image);
}
