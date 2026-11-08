#include "panel_input.hpp"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
static unsigned checks=0;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);++checks;}
int main(){try{
    for(const char* port:{"hw:0,0,0","hw:1,0,1","hw:123,45,6"})check(pmu::PanelInput::validPort(port),"valid hardware port rejected");
    for(const char* port:{"","hw:","hw:1,0","hw:1,0,1x","hw:1,0,1,0","hw:-1,0,1","hw:1000,0,1","default","hw:1,,1"})check(!pmu::PanelInput::validPort(port),"invalid hardware port accepted");
    check(!pmu::PanelInput::validPort(nullptr),"missing port accepted");
    {
        namespace fs=std::filesystem;char pattern[]="/tmp/pmu-panel-profile.XXXXXX";const char* created=mkdtemp(pattern);check(created,"profile fixture failed");const fs::path root=created;
        fs::create_directories(root/"class/sound/card1");fs::create_directories(root/"devices/usb/panel/interface");fs::create_directory_symlink(root/"devices/usb/panel/interface",root/"class/sound/card1/device");
        auto write=[&](const char* name,const std::string& value){std::ofstream(root/"devices/usb/panel"/name)<<value;};
        write("idVendor","09e8\n");write("idProduct","0047\n");check(pmu::PanelInput::deviceModel("hw:1,0,1",root)==0x47,"Live II profile was not derived from the selected ALSA card");
        write("idProduct","0046\n");check(pmu::PanelInput::deviceModel("hw:1,0,1",root)==0,"unverified panel model was activated");write("idProduct","0047\n");write("idVendor","other\n");check(pmu::PanelInput::deviceModel("hw:1,0,1",root)==0,"unrelated MIDI hardware was activated");
        check(pmu::PanelInput::deviceModel(nullptr,root)==0&&pmu::PanelInput::deviceModel("hw:2,0,0",root)==0,"missing panel identity did not fall back safely");fs::remove_all(root);
    }
    pmu::WheelMessages wheel;std::vector<int> actions;auto action=[&](int value){actions.push_back(value);};
    for(auto value:{1,3,127,125,0})wheel.feed(0xb0,100,value,action);
    check(actions==std::vector<int>{1,3,-1,-3},"firmware wheel signed deltas changed");
    actions.clear();wheel.feed(0x90,111,127,action);wheel.feed(0x90,111,127,action);wheel.feed(0x90,111,0,action);wheel.feed(0x90,111,1,action);wheel.feed(0x80,111,64,action);wheel.feed(0x90,111,127,action);check(actions==std::vector<int>{0,0,0},"wheel presses repeat or releases activate a card");
    actions.clear();for(auto status:{0xb1,0x91,0x81})wheel.feed(status,100,1,action);wheel.feed(0xb0,101,1,action);wheel.feed(0x90,49,127,action);wheel.feed(0x90,123,127,action);wheel.feed(0x90,36,127,action);wheel.feed(0x90,58,127,action);check(actions.empty(),"unrelated controls activated wheel navigation");
    actions.clear();for(unsigned note=0;note<128;++note)if(note!=111){wheel.feed(0x90,note,127,action);wheel.feed(0x80,note,0,action);}for(unsigned cc=0;cc<128;++cc)if(cc!=100)wheel.feed(0xb0,cc,1,action);check(actions.empty(),"non-wheel panel controls triggered manager actions");
    {
        pmu::PanelMessages stream;std::vector<int> values;
        auto decode=[decoder=pmu::WheelMessages{},&values](unsigned char status,unsigned char number,unsigned char value)mutable{decoder.feed(status,number,value,[&](int delta){values.push_back(delta);});};
        for(auto byte:{0x90,111,127,111,127,111,0,111,127})stream.feed(byte,decode);
        check(values==std::vector<int>{0,0},"stream callback lost held-press state between MIDI messages");
    }
    pmu::PanelMessages parser;std::vector<std::array<unsigned char,3>> messages;
    auto receive=[&](unsigned char a,unsigned char b,unsigned char c){messages.push_back({a,b,c});};
    const unsigned char stream[]={0x01,0xb0,0x20,0xf8,0x01,0x20,0x7f,0x90,0x15,0x7f,0x15,0,0xf0,0x01,0x02,0xf8,0xf7,0x20,0x01,0xc1,0x04,0x05,0xf1,0x02,0x03,0xb2,0x03,0xff,0x04};
    for(auto byte:stream)parser.feed(byte,receive);
    check(messages==std::vector<std::array<unsigned char,3>>{{0xb0,0x20,1},{0xb0,0x20,0x7f},{0x90,0x15,0x7f},{0x90,0x15,0},{0xc1,4,0},{0xc1,5,0},{0xb2,3,4}},"MIDI framing, running status or realtime handling failed");
    void* library=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);check(library,"fake ALSA unavailable");
    auto send=reinterpret_cast<ssize_t(*)(const void*,size_t)>(dlsym(library,"panel_test_send"));auto closed=reinterpret_cast<int(*)()>(dlsym(library,"panel_test_closed"));check(send&&closed,"fake ALSA controls unavailable");
    auto output=reinterpret_cast<size_t(*)(void*,size_t)>(dlsym(library,"panel_test_output"));auto reset=reinterpret_cast<void(*)(int)>(dlsym(library,"panel_test_reset"));check(output&&reset,"fake output controls unavailable");
    reset(0);
    {pmu::PanelInput input("hw:1,0,1",receive,0x47);check(input.active(),"panel input 1 and configuration output 0 did not open separately");unsigned char packet[32]{};const auto size=output(packet,sizeof packet);check(size==9&&std::vector<unsigned char>(packet,packet+size)==std::vector<unsigned char>{0xf0,0x47,0x7f,0x47,0x62,0,1,5,0xf7},"activation differs from the verified stock mode command");}
    check(closed(),"duplex panel handles remained open after return");
    reset(-ENODEV);{pmu::PanelInput input("hw:1,0,1",receive,0x47);check(!input.active()&&closed(),"failed activation retained hardware ownership");}
    reset(-EAGAIN);const auto blocked=std::chrono::steady_clock::now();{pmu::PanelInput input("hw:1,0,1",receive,0x47);check(!input.active()&&closed(),"blocked activation retained hardware ownership");}check(std::chrono::steady_clock::now()-blocked<std::chrono::seconds(1),"blocked panel output delayed return");
    reset(0);{pmu::PanelInput input("hw:1,0,1",receive,0x40);check(!input.active()&&closed(),"unsupported activation model reached the MIDI hardware");unsigned char packet[32]{};check(output(packet,sizeof packet)==0,"unsupported hardware received an activation packet");}
    std::mutex mutex;std::condition_variable ready;messages.clear();
    const auto before=std::chrono::steady_clock::now();
    {
        pmu::PanelInput input("hw:1,0,1",[&](unsigned char a,unsigned char b,unsigned char c){std::lock_guard<std::mutex> lock(mutex);messages.push_back({a,b,c});ready.notify_one();});
        check(input.active(),"input did not open");const unsigned char first[]={0xb0,0x20};check(send(first,sizeof first)==2,"first packet failed");const unsigned char second[]={1,0x20,0x7f};check(send(second,sizeof second)==3,"second packet failed");
        std::unique_lock<std::mutex> lock(mutex);check(ready.wait_for(lock,std::chrono::seconds(2),[&]{return messages.size()==2;}),"event-driven input failed");check(messages[0][2]==1&&messages[1][2]==0x7f,"fragmented input changed");
    }
    check(closed(),"MIDI handle was not closed");check(std::chrono::steady_clock::now()-before<std::chrono::seconds(2),"idle input shutdown hung");
    for(int i=0;i<20;++i){pmu::PanelInput input("hw:1,0,1",receive);check(input.active(),"input could not reopen");}
    check(closed(),"repeated open/close leaked input");
    {pmu::PanelInput missing(nullptr,receive);check(!missing.active(),"missing input started a worker");pmu::PanelInput unavailable("hw:2,0,0",receive);check(!unavailable.active(),"failed ALSA input started a worker");}
    dlclose(library);std::cout<<checks<<" panel input checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
