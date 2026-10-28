#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

// The manager draws a fixed 1280x800 canvas. A portrait panel (the Gen 2 DSI panels) shows it rotated 270 degrees.
// A panel mounted the other way up needs the picture and the touch mapping turned a further 180 degrees; MPC takes that
// from the device tree (inmusic,panel-rotation), which this manager does not decode yet, so for now it is a setting.
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

// display-rotation.txt on the USB stick: "180" turns the picture and touch upside down, "0" (or an empty file) leaves the
// automatic choice. Anything else is ignored and named in `note` so the result log shows why.
inline bool displayFlipFromSetting(const std::string& text, std::string& note) {
    size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    const std::string value = first == std::string::npos ? "" : text.substr(first, last - first + 1);
    if(value == "180") return true;
    if(value.empty() || value == "0") return false;
    note = "ignored display-rotation.txt value \"" + value.substr(0, 16) + "\" (use 0 or 180)";
    return false;
}

// The raw bytes of the device tree property MPC itself uses for the panel's orientation, for the result log. Empty when
// the property does not exist (a Live III has none).
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
