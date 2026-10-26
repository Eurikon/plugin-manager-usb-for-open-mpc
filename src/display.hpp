#pragma once
#include <algorithm>
#include <utility>

struct DisplayGeometry {
    bool portrait;
    int width() const { return portrait ? 800 : 1280; }
    int height() const { return portrait ? 1280 : 800; }
    std::pair<int,int> input(float x,float y) const {
        return {std::clamp(int(portrait ? 1280-y : x),0,1279),
                std::clamp(int(portrait ? x : y),0,799)};
    }
    std::pair<int,int> output(int x,int y) const {
        return portrait ? std::make_pair(y,1280-x) : std::make_pair(x,y);
    }
};
