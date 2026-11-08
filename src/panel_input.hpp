#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace pmu {
struct WheelMessages {
    bool pressed=false;
    template<class Callback> void feed(unsigned char status,unsigned char number,unsigned char value,Callback callback){
        if(status==0xb0&&number==100){const int delta=value>64?int(value)-128:int(value);if(delta)callback(delta);}
        else if((status==0x90||status==0x80)&&number==111){const bool down=status==0x90&&value!=0;if(down&&!pressed)callback(0);pressed=down;}
    }
};
struct PanelMessages {
    unsigned char status=0,data[2]{},count=0;
    bool sysex=false;
    template<class Callback> void feed(unsigned char byte,Callback&& callback){
        if(byte>=0xf8)return;
        if(byte&0x80){count=0;if(byte==0xf7){sysex=false;status=0;}else if(byte==0xf0){sysex=true;status=0;}else{sysex=false;status=byte<0xf0?byte:0;}return;}
        if(sysex||!status)return;
        data[count++]=byte;
        const unsigned needed=(status&0xf0)==0xc0||(status&0xf0)==0xd0?1:2;
        if(count<needed)return;
        count=0;callback(status,data[0],needed==2?data[1]:0);
    }
};
class PanelInput {
    struct RawMidi;
    void* library=nullptr;
    RawMidi* input=nullptr;
    RawMidi* output=nullptr;
    unsigned char model=0;
    int wake=-1;
    int (*closeInput)(RawMidi*)=nullptr;
    ssize_t (*readInput)(RawMidi*,void*,size_t)=nullptr;
    ssize_t (*writeOutput)(RawMidi*,const void*,size_t)=nullptr;
    int (*descriptorCount)(RawMidi*)=nullptr;
    int (*getDescriptors)(RawMidi*,pollfd*,unsigned)=nullptr;
    int (*inputEvents)(RawMidi*,pollfd*,unsigned,unsigned short*)=nullptr;
    std::array<pollfd,17> descriptors{};
    unsigned count=0;
    std::thread worker;
    template<class T> bool symbol(T& value,const char* name){value=reinterpret_cast<T>(dlsym(library,name));return value!=nullptr;}
    bool sendMode(unsigned char mode){
        const unsigned char packet[]={0xf0,0x47,0x7f,model,0x62,0,1,mode,0xf7};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        size_t sent=0;
        while(sent<sizeof packet&&std::chrono::steady_clock::now()<deadline){
            const ssize_t size=writeOutput(output,packet+sent,sizeof packet-sent);
            if(size>0){sent+=size_t(size);continue;}
            if(size<0&&size!=-EAGAIN&&size!=-EINTR)return false;
            poll(nullptr,0,1);
        }
        return sent==sizeof packet;
    }
    void release(){if(input&&closeInput)closeInput(input);input=nullptr;if(output&&closeInput)closeInput(output);output=nullptr;if(wake>=0)close(wake);wake=-1;if(library)dlclose(library);library=nullptr;}
public:
    static bool validPort(const char* port){
        if(!port||std::strncmp(port,"hw:",3)!=0)return false;
        const char* at=port+3;
        for(int part=0;part<3;++part){int digits=0;while(*at>='0'&&*at<='9'){++at;if(++digits>3)return false;}if(!digits)return false;if(part<2){if(*at++!=',')return false;}else if(*at)return false;}
        return true;
    }
    static unsigned char deviceModel(const char* port,const std::filesystem::path& sys="/sys"){
        if(!validPort(port))return 0;
        const std::string value=port;const auto card=value.substr(3,value.find(',')-3);
        std::error_code error;const auto root=std::filesystem::canonical(sys,error);if(error)return 0;
        auto path=std::filesystem::canonical(root/"class/sound"/("card"+card)/"device",error);if(error)return 0;
        const auto relative=path.lexically_relative(root);if(relative.empty()||*relative.begin()=="..")return 0;
        auto text=[](const std::filesystem::path& file){std::ifstream stream(file);char data[16]{};return stream.getline(data,sizeof data)?std::string(data):std::string();};
        for(unsigned level=0;level<8&&path!=root;++level,path=path.parent_path()){
            const auto vendor=text(path/"idVendor");if(vendor.empty())continue;
            return vendor=="09e8"&&text(path/"idProduct")=="0047"?0x47:0;
        }
        return 0;
    }
    PanelInput(const char* port,std::function<void(unsigned char,unsigned char,unsigned char)> callback,unsigned char panelModel=0):model(panelModel){
        if(!validPort(port))return;
        library=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);if(!library)return;
        int (*openInput)(RawMidi**,RawMidi**,const char*,int)=nullptr;
        if(!symbol(openInput,"snd_rawmidi_open")||!symbol(closeInput,"snd_rawmidi_close")||!symbol(readInput,"snd_rawmidi_read")||!symbol(inputEvents,"snd_rawmidi_poll_descriptors_revents")||!symbol(descriptorCount,"snd_rawmidi_poll_descriptors_count")||!symbol(getDescriptors,"snd_rawmidi_poll_descriptors")||((model!=0&&model!=0x47))||(model&&!symbol(writeOutput,"snd_rawmidi_write"))||openInput(&input,nullptr,port,0x0002)<0){release();return;}
        if(model){const std::string controlPort=std::string(port).substr(0,std::string(port).rfind(',')+1)+"0";if(openInput(nullptr,&output,controlPort.c_str(),0x0002)<0){release();return;}}
        const int found=descriptorCount(input);
        if(found<1||found>16){release();return;}
        count=unsigned(found);
        if(getDescriptors(input,descriptors.data(),count)!=found){release();return;}
        for(unsigned i=0;i<count;++i)if(descriptors[i].fd<0||fcntl(descriptors[i].fd,F_SETFD,FD_CLOEXEC)<0){release();return;}
        wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);if(wake<0){release();return;}
        descriptors[count]={wake,POLLIN,0};
        if(output){
            std::array<pollfd,16> writeDescriptors{};const int foundOutput=descriptorCount(output);
            if(foundOutput<1||foundOutput>16||getDescriptors(output,writeDescriptors.data(),unsigned(foundOutput))!=foundOutput){release();return;}
            for(int i=0;i<foundOutput;++i)if(writeDescriptors[i].fd<0||fcntl(writeDescriptors[i].fd,F_SETFD,FD_CLOEXEC)<0){release();return;}
            if(!sendMode(5)){release();return;}
        }
        try{worker=std::thread([this,callback]{
            PanelMessages messages;
            for(;;){
                const int ready=poll(descriptors.data(),count+1,-1);if(ready<0){if(errno==EINTR)continue;break;}
                if(descriptors[count].revents)break;
                unsigned short events=0;if(inputEvents(input,descriptors.data(),count,&events)<0||events&(POLLERR|POLLHUP|POLLNVAL))break;
                if(!(events&POLLIN))continue;
                for(unsigned batch=0;batch<16;++batch){unsigned char bytes[256];const ssize_t size=readInput(input,bytes,sizeof bytes);if(size==-EINTR)continue;if(size==-EAGAIN)break;if(size<=0)return;for(ssize_t i=0;i<size;++i)messages.feed(bytes[i],callback);}
            }
        });}catch(...){release();throw;}
    }
    ~PanelInput(){if(worker.joinable()){const uint64_t stop=1;ssize_t result;do{result=write(wake,&stop,sizeof stop);}while(result<0&&errno==EINTR);worker.join();}release();}
    PanelInput(const PanelInput&)=delete;
    PanelInput& operator=(const PanelInput&)=delete;
    bool active()const{return worker.joinable();}
};
}
