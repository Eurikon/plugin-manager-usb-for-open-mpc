#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <utility>

struct DisplayGeometry {
    bool portrait;
    bool flipped = false;
    int width() const { return portrait ? 800 : 1280; }
    int height() const { return portrait ? 1280 : 800; }
    std::pair<int,int> input(float x,float y) const {
        if(flipped) { x = width() - x; y = height() - y; }
        return {std::clamp(int(portrait ? 1280-y : x),0,1279),
                std::clamp(int(portrait ? x : y),0,799)};
    }
    std::pair<int,int> output(int x,int y) const {
        const auto point = portrait ? std::make_pair(y,1280-x) : std::make_pair(x,y);
        return flipped ? std::make_pair(width()-point.first,height()-point.second) : point;
    }
};

inline bool displayFlipFromSetting(const std::string& text, std::string& note) {
    note.clear();
    size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    const std::string value = first == std::string::npos ? "" : text.substr(first, last - first + 1);
    if(value == "180") return true;
    if(value.empty() || value == "0") return false;
    note = "ignored display-rotation.txt value \"" + value.substr(0, 16) + "\" (use 0 or 180)";
    return false;
}

inline bool displayFlipFromFile(const std::string& path, std::string& note) {
    note.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) {
        if(errno != ENOENT) note = "ignored unreadable display-rotation.txt";
        return false;
    }
    struct stat info{};
    char buffer[65];
    size_t size = 0;
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= 64;
    while(valid && size < sizeof buffer) {
        const auto count = read(fd, buffer + size, sizeof buffer - size);
        if(count < 0 && errno == EINTR) continue;
        if(count < 0) { valid = false; break; }
        if(count == 0) break;
        size += size_t(count);
    }
    close(fd);
    if(!valid || size > 64) {
        note = "ignored invalid display-rotation.txt (use a regular file of at most 64 bytes)";
        return false;
    }
    return displayFlipFromSetting(std::string(buffer, size), note);
}

inline std::string devicetreePanelRotationHex(const char* path = "/sys/firmware/devicetree/base/inmusic,panel-rotation") {
    std::string hex;
    if(FILE* file = std::fopen(path, "rb")) {
        unsigned char byte;
        char text[4];
        for(int count = 0; count < 16 && std::fread(&byte, 1, 1, file) == 1; ++count) {
            std::snprintf(text, sizeof text, "%02x", byte);
            hex += (hex.empty() ? "" : " ") + std::string(text);
        }
        std::fclose(file);
    }
    return hex;
}
