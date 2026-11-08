#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
struct Input{int fds[2];bool output;};
static std::mutex state;
static std::condition_variable changed;
static Input* active=nullptr;
static Input* outgoing=nullptr;
static std::vector<unsigned char> sent;
static int writeError=0;
static bool armed=false,awake=false;
static unsigned dropped=0,busy=0,replyStatus=2,queries=0,activations=0;
static std::chrono::steady_clock::time_point lastTraffic;
static bool fresh(){return awake&&std::chrono::steady_clock::now()-lastTraffic<std::chrono::seconds(10);}
extern "C" {
int poll(pollfd* descriptors,nfds_t count,int timeout){
    {std::lock_guard<std::mutex> lock(state);if(active)for(nfds_t i=0;i<count;++i)if(descriptors[i].fd==active->fds[0]&&(descriptors[i].events&POLLIN))armed=true;}
#ifdef SYS_poll
    return int(syscall(SYS_poll,descriptors,count,timeout));
#else
    timespec duration{};if(timeout>=0){duration.tv_sec=timeout/1000;duration.tv_nsec=(timeout%1000)*1000000L;}
    return int(syscall(SYS_ppoll,descriptors,count,timeout<0?nullptr:&duration,nullptr,0));
#endif
}
int snd_rawmidi_open(Input** input,Input** output,const char* port,int mode){
    std::lock_guard<std::mutex> lock(state);
    if(mode!=2||!port||(!input&&!output)||(input&&output))return -EINVAL;
    if(input){
        if(std::strcmp(port,"hw:1,0,1"))return -EINVAL;
        auto value=new Input{{-1,-1},false};if(pipe2(value->fds,O_NONBLOCK|O_CLOEXEC)){delete value;return -errno;}
        *input=active=value;armed=awake=false;
    }else{
        if(std::strcmp(port,"hw:1,0,0"))return -EINVAL;
        auto value=new Input{{-1,-1},true};if(pipe2(value->fds,O_NONBLOCK|O_CLOEXEC)){delete value;return -errno;}*output=outgoing=value;
    }
    return 0;
}
int snd_rawmidi_close(Input* input){std::lock_guard<std::mutex> lock(state);close(input->fds[0]);close(input->fds[1]);if(input->output)outgoing=nullptr;else active=nullptr;delete input;return 0;}
int snd_rawmidi_poll_descriptors_count(Input*){return 1;}
int snd_rawmidi_poll_descriptors(Input* input,pollfd* descriptors,unsigned count){if(count!=1)return -EINVAL;descriptors[0]={input->fds[0],POLLIN,0};return 1;}
int snd_rawmidi_poll_descriptors_revents(Input*,pollfd* descriptors,unsigned count,unsigned short* events){if(count!=1)return -EINVAL;*events=descriptors[0].revents;return 0;}
ssize_t snd_rawmidi_read(Input* input,void* data,size_t size){const auto result=read(input->fds[0],data,size);return result<0?-errno:result;}
ssize_t snd_rawmidi_write(Input* input,const void* data,size_t size){
    std::lock_guard<std::mutex> lock(state);
    if(!input->output)return -EINVAL;
    if(writeError)return writeError;
    const auto bytes=static_cast<const unsigned char*>(data);sent.insert(sent.end(),bytes,bytes+size);
    const unsigned char query[]={0xf0,0x47,0,0x47,0x52,0,4};
    const unsigned char activation[]={0xf0,0x47,0x7f,0x47,0x62,0,1,0x65,0xf7};
    if(size==12&&!std::memcmp(bytes,query,sizeof query)&&bytes[11]==0xf7){
        ++queries;
        if(armed){
            lastTraffic=std::chrono::steady_clock::now();
            if(busy){--busy;const unsigned char response[]={0xf0,0x47,0x7f,0x47,0x53,0,5,0x11,0x22,0x33,0x44,0,0xf7};if(write(active->fds[1],response,sizeof response)!=ssize_t(sizeof response))return -EIO;}
            else if(dropped)--dropped;
            else{const unsigned char response[]={0xf0,0x47,0x7f,0x47,0x53,0,5,bytes[7],bytes[8],bytes[9],bytes[10],static_cast<unsigned char>(replyStatus),0xf7};if(write(active->fds[1],response,sizeof response)!=ssize_t(sizeof response))return -EIO;}
        }
    }else if(size==sizeof activation&&!std::memcmp(bytes,activation,size)&&armed){awake=true;lastTraffic=std::chrono::steady_clock::now();++activations;}
    changed.notify_all();return ssize_t(size);
}
ssize_t panel_test_send(const void* data,size_t size){std::lock_guard<std::mutex> lock(state);return active&&(!std::getenv("MPC_MANAGER_PANEL_FIXTURE")||fresh())?write(active->fds[1],data,size):-EPIPE;}
int panel_test_closed(){std::lock_guard<std::mutex> lock(state);return active==nullptr&&outgoing==nullptr;}
size_t panel_test_output(void* data,size_t size){std::lock_guard<std::mutex> lock(state);if(size<sent.size())return size_t(-1);std::memcpy(data,sent.data(),sent.size());return sent.size();}
void panel_test_reset(int error){std::lock_guard<std::mutex> lock(state);sent.clear();writeError=error;queries=activations=dropped=busy=0;replyStatus=2;}
void panel_test_drop(unsigned number){std::lock_guard<std::mutex> lock(state);dropped=number;}
void panel_test_busy(unsigned number){std::lock_guard<std::mutex> lock(state);busy=number;}
void panel_test_status(unsigned status){std::lock_guard<std::mutex> lock(state);replyStatus=status;}
int panel_test_wait_active(unsigned timeout){std::unique_lock<std::mutex> lock(state);return changed.wait_for(lock,std::chrono::milliseconds(timeout),[]{return fresh();});}
unsigned panel_test_queries(){std::lock_guard<std::mutex> lock(state);return queries;}
unsigned panel_test_activations(){std::lock_guard<std::mutex> lock(state);return activations;}
}
