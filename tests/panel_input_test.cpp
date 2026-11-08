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
    {
        const std::array<unsigned char,13> reply={0xf0,0x47,0x7f,0x47,0x53,0,5,1,2,3,4,1,0xf7};
        auto parse=[&](const auto& bytes){pmu::PanelReplies parser;parser.tag={1,2,3,4};int result=0;for(auto byte:bytes)result=parser.feed(byte);return result;};
        check(parse(reply)==1,"ready panel reply rejected");
        auto changed=reply;changed[11]=2;check(parse(changed)==2,"panel restart reply rejected");
        for(unsigned index:{1,2,3,4,5,6,7,8,9,10,11}){changed=reply;changed[index]=0x20;check(parse(changed)==0,"mismatched panel reply accepted");}
        std::vector<unsigned char> realtime;for(auto byte:reply){realtime.push_back(0xf8);realtime.push_back(byte);}check(parse(realtime)==1,"realtime bytes interrupted panel reply");
        std::vector<unsigned char> shortReply(reply.begin(),reply.end());shortReply.erase(shortReply.begin()+8);check(parse(shortReply)==0,"short panel reply accepted");
        std::vector<unsigned char> longReply(reply.begin(),reply.end());longReply.insert(longReply.end()-1,0);check(parse(longReply)==0,"long panel reply accepted");
        std::vector<unsigned char> interrupted(reply.begin(),reply.begin()+8);interrupted.push_back(0x90);interrupted.insert(interrupted.end(),reply.begin()+8,reply.end());check(parse(interrupted)==0,"interrupted panel reply accepted");
        interrupted.insert(interrupted.end(),reply.begin(),reply.end());check(parse(interrupted)==1,"panel parser did not recover after interruption");
        std::vector<unsigned char> nested(reply.begin(),reply.begin()+8);nested.insert(nested.end(),reply.begin(),reply.end());check(parse(nested)==1,"panel parser did not recover on new SysEx start");
    }
    void* library=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);check(library,"fake ALSA unavailable");
    auto send=reinterpret_cast<ssize_t(*)(const void*,size_t)>(dlsym(library,"panel_test_send"));auto closed=reinterpret_cast<int(*)()>(dlsym(library,"panel_test_closed"));check(send&&closed,"fake ALSA controls unavailable");
    auto output=reinterpret_cast<size_t(*)(void*,size_t)>(dlsym(library,"panel_test_output"));auto reset=reinterpret_cast<void(*)(int)>(dlsym(library,"panel_test_reset"));check(output&&reset,"fake output controls unavailable");
    auto waitActive=reinterpret_cast<int(*)(unsigned)>(dlsym(library,"panel_test_wait_active"));
    auto drop=reinterpret_cast<void(*)(unsigned)>(dlsym(library,"panel_test_drop"));
    auto busy=reinterpret_cast<void(*)(unsigned)>(dlsym(library,"panel_test_busy"));
    auto status=reinterpret_cast<void(*)(unsigned)>(dlsym(library,"panel_test_status"));
    auto queries=reinterpret_cast<unsigned(*)()>(dlsym(library,"panel_test_queries"));
    auto activations=reinterpret_cast<unsigned(*)()>(dlsym(library,"panel_test_activations"));
    check(waitActive&&drop&&busy&&status&&queries&&activations,"panel lifecycle controls unavailable");
    reset(0);
    {
        pmu::PanelInput input("hw:1,0,1",receive,0x47);
        check(input.active()&&waitActive(1000),"armed panel input and configuration output did not initialize");
        unsigned char packet[64]{};const auto size=output(packet,sizeof packet);
        check(size==27&&std::vector<unsigned char>(packet,packet+6)==std::vector<unsigned char>{0xf0,0x7e,0,6,1,0xf7}&&packet[6]==0xf0&&packet[10]==0x52&&packet[17]==0xf7,"stock identity and panel query must precede activation");
        check(std::vector<unsigned char>(packet+18,packet+size)==std::vector<unsigned char>{0xf0,0x47,0x7f,0x47,0x62,0,1,0x65,0xf7},"activation differs from the verified stock mode command");
    }
    check(closed(),"duplex panel handles remained open after return");
    reset(-ENODEV);{pmu::PanelInput input("hw:1,0,1",receive,0x47);check(!input.active()&&closed(),"failed activation retained hardware ownership");}
    reset(-EAGAIN);const auto blocked=std::chrono::steady_clock::now();{pmu::PanelInput input("hw:1,0,1",receive,0x47);check(!input.active()&&closed(),"blocked activation retained hardware ownership");}check(std::chrono::steady_clock::now()-blocked<std::chrono::seconds(1),"blocked panel output delayed return");
    reset(0);{pmu::PanelInput input("hw:1,0,1",receive,0x40);check(!input.active()&&closed(),"unsupported activation model reached the MIDI hardware");unsigned char packet[32]{};check(output(packet,sizeof packet)==0,"unsupported hardware received an activation packet");}
    reset(0);drop(2);
    {pmu::PanelInput input("hw:1,0,1",receive,0x47);check(waitActive(3500),"panel did not recover from dropped startup replies");check(queries()==3&&activations()==1,"startup retried without matching readiness");}
    check(closed(),"retry session leaked panel handles");
    reset(0);busy(6);
    {pmu::PanelInput input("hw:1,0,1",receive,0x47);check(waitActive(7500),"panel did not wait for the former native session to expire");check(queries()==7&&activations()==1,"busy native session was mistaken for matching readiness");}
    check(closed(),"native handover leaked panel handles");
    reset(0);drop(100);
    {pmu::PanelInput input("hw:1,0,1",receive,0x47);std::this_thread::sleep_for(std::chrono::milliseconds(20250));check(!input.active()&&queries()==20&&activations()==0,"unresponsive panel exceeded bounded startup retries");}
    check(closed(),"failed readiness leaked panel handles");
    reset(0);setenv("MPC_MANAGER_PANEL_FIXTURE","1",1);
    {
        auto open=reinterpret_cast<int(*)(void**,void**,const char*,int)>(dlsym(library,"snd_rawmidi_open"));
        auto close=reinterpret_cast<int(*)(void*)>(dlsym(library,"snd_rawmidi_close"));
        auto descriptors=reinterpret_cast<int(*)(void*,pollfd*,unsigned)>(dlsym(library,"snd_rawmidi_poll_descriptors"));
        auto write=reinterpret_cast<ssize_t(*)(void*,const void*,size_t)>(dlsym(library,"snd_rawmidi_write"));
        check(open&&close&&descriptors&&write,"watchdog fixture API unavailable");
        void* input=nullptr;void* output=nullptr;pollfd descriptor{};
        check(open(&input,nullptr,"hw:1,0,1",2)==0&&open(nullptr,&output,"hw:1,0,0",2)==0,"watchdog fixture did not open");
        check(descriptors(input,&descriptor,1)==1&&poll(&descriptor,1,0)==0,"watchdog fixture input did not arm");
        const unsigned char mode[]={0xf0,0x47,0x7f,0x47,0x62,0,1,0x65,0xf7};
        check(write(output,mode,sizeof mode)==ssize_t(sizeof mode)&&waitActive(100),"one-shot activation did not work initially");
        std::this_thread::sleep_for(std::chrono::milliseconds(10250));const unsigned char wheel[]={0xb0,100,1};
        check(send(wheel,sizeof wheel)==-EPIPE,"fixture failed to reproduce the controller watchdog cutoff");
        check(close(input)==0&&close(output)==0&&closed(),"watchdog fixture handles remained open");
    }
    reset(0);status(1);setenv("MPC_MANAGER_PANEL_FIXTURE","1",1);
    {
        std::mutex guard;std::condition_variable notified;std::vector<int> values;
        auto decode=[decoder=pmu::WheelMessages{},&values,&guard,&notified](unsigned char a,unsigned char b,unsigned char c)mutable{std::lock_guard<std::mutex> lock(guard);decoder.feed(a,b,c,[&](int value){values.push_back(value);});notified.notify_one();};
        pmu::PanelInput input("hw:1,0,1",decode,0x47);check(waitActive(1000),"sustained panel session did not activate");
        std::this_thread::sleep_for(std::chrono::seconds(11));
        check(queries()>=3&&activations()==1,"ready panel requires recurring probes without blind activation");
        {std::lock_guard<std::mutex> lock(guard);check(values.empty(),"keepalive triggered a user action");}
        const unsigned char wheel[]={0xb0,100,1,100,127,0x90,111,127,111,0};
        check(send(wheel,sizeof wheel)==ssize_t(sizeof wheel),"wheel stopped after the controller watchdog");
        {std::unique_lock<std::mutex> lock(guard);check(notified.wait_for(lock,std::chrono::seconds(1),[&]{return values.size()==3;})&&values==std::vector<int>{1,-1,0},"sustained rotation and press changed");}
        status(2);const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(activations()<2&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(activations()==2,"panel restart reply did not restore activation");
    }
    check(closed(),"sustained session retained hardware after return");
    const auto stopped=queries();std::this_thread::sleep_for(std::chrono::milliseconds(100));check(queries()==stopped,"keepalive outlived the manager session");
    unsetenv("MPC_MANAGER_PANEL_FIXTURE");reset(0);
    auto descriptorCount=[](){return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});};
    const auto descriptorsBefore=descriptorCount();
    for(int i=0;i<20;++i){pmu::PanelInput input("hw:1,0,1",receive,0x47);check(input.active()&&waitActive(1000),"configured panel could not reopen");}
    check(closed(),"configured reopen retained MIDI handles");
    check(descriptorCount()==descriptorsBefore,"configured reopen leaked timer, wake or MIDI descriptors");
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
