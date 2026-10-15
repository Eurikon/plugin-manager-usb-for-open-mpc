#include "../src/shortcut_state.hpp"
#include <cassert>
#include <iostream>
#include <utility>
#include <vector>
int main(){
    HoldShortcut hold;hold.button(49,true,100);hold.button(123,true,200);
    assert(hold.deadline==2200&&!hold.expire(2199)&&hold.expire(2200)&&!hold.expire(3000));
    hold.button(123,true,3100);assert(!hold.deadline);hold.button(49,false,3200);assert(!hold.fired&&!hold.deadline);
    hold.button(49,true,3300);hold.button(49,true,3500);assert(hold.deadline==5300);hold.button(123,false,5299);assert(!hold.expire(5300));
    hold={};hold.button(123,true,1);hold.button(49,true,10);hold.button(60,true,11);assert(hold.deadline==2010&&hold.expire(2010));
    hold={};hold.button(49,true,19669);hold.button(123,true,20898);hold.button(123,false,21004);hold.button(49,false,21319);assert(!hold.expire(23960));
    hold.button(49,true,23960);hold.button(123,true,24680);assert(!hold.expire(26679)&&hold.expire(26680));hold.button(123,false,30935);hold.button(49,false,30935);
    PanelMidi parser;std::vector<std::pair<unsigned char,bool>> events;
    auto receive=[&](unsigned char note,bool down){events.emplace_back(note,down);};
    for(unsigned char byte:{0x90,49,0xf8,127,123,127,49,0,0x80,123,0})parser.feed(byte,receive);
    assert(events.size()==4&&events[0]==std::make_pair((unsigned char)49,true)&&!events[2].second&&!events[3].second);
    events.clear();for(unsigned char byte:{0xf0,0x90,49,127,0xf7,0x91,49,127,0x90,60,127,0xc0,49,0x90,49})parser.feed(byte,receive);
    assert(events.empty());parser.feed(127,receive);assert(events.size()==1&&events[0].second);
    std::cout<<"PASS: two-second boundary, cancellation, repeat protection, both press orders, running status, split messages, realtime, SysEx and unrelated input\n";
}
