#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <vector>

struct Thumbnail {
    int width=0,height=0;
    std::vector<uint32_t> pixels;
};

class ThumbnailCache {
public:
    using Cancelled=std::function<bool()>;
    using Fetch=std::function<std::vector<unsigned char>(const std::string&,const Cancelled&)>;
    static constexpr size_t inputLimit=1024*1024,pixelLimit=1024*1024,cacheLimit=4*1024*1024;
    static constexpr int width=128,height=80,dimensionLimit=2048;
    explicit ThumbnailCache(Fetch fetch={});
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&)=delete;
    ThumbnailCache& operator=(const ThumbnailCache&)=delete;
    void visible(const std::vector<std::string>& urls);
    std::shared_ptr<const Thumbnail> get(const std::string& url);
    uint64_t revision() const {return changed.load();}
    void clear();
    size_t bytes() const;
    static bool validUrl(const std::string& url);
    static Thumbnail decode(const std::vector<unsigned char>& encoded);
private:
    struct Entry {std::shared_ptr<const Thumbnail> image;uint64_t used=0;};
    Fetch fetch;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::string> wanted;
    std::map<std::string,Entry> cache;
    std::set<std::string> failed;
    std::thread worker;
    std::atomic<bool> stopped{false};
    std::atomic<uint64_t> changed{0};
    uint64_t clock=0,generation=0;
    size_t pixelBytes=0;
    void run();
};
