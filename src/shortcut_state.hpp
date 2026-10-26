#pragma once
#include <cstdint>

struct PanelMidi {
    unsigned char status=0,data[2]{},count=0;
    bool sysex=false;
    template<class Callback> void feed(unsigned char byte,Callback callback) {
        if(byte>=0xf8)return;
        if(sysex){if(byte==0xf7)sysex=false;return;}
        if(byte==0xf0){sysex=true;status=0;count=0;return;}
        if(byte&0x80){status=byte<0xf0?byte:0;count=0;return;}
        if(!status)return;
        data[count++]=byte;
        const unsigned needed=(status&0xf0)==0xc0||(status&0xf0)==0xd0?1:2;
        if(count<needed)return;
        count=0;
        if((status==0x90||status==0x80)&&(data[0]==49||data[0]==123))callback(data[0],status==0x90&&data[1]!=0);
    }
};
struct HoldShortcut {
    bool shift=false,menu=false,fired=false;
    uint64_t deadline=0;
    void button(unsigned char note,bool down,uint64_t now) {
        if(note==49)shift=down;else if(note==123)menu=down;else return;
        if(!shift||!menu){deadline=0;fired=false;}
        else if(!deadline&&!fired)deadline=now+2000;
    }
    bool expire(uint64_t now) {
        if(!deadline||now<deadline||!shift||!menu||fired)return false;
        deadline=0;fired=true;return true;
    }
};
