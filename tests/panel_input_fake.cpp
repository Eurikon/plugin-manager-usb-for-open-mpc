#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <vector>
struct Input{int fds[2];bool output;};
static Input* active=nullptr;
static Input* outgoing=nullptr;
static std::vector<unsigned char> sent;
static int writeError=0;
static bool awake=false;
extern "C" {
int snd_rawmidi_open(Input** input,Input** output,const char* port,int mode){
    if(mode!=2||!port||(!input&&!output)||(input&&output))return -EINVAL;
    if(input){
        if(std::strcmp(port,"hw:1,0,1"))return -EINVAL;
        auto value=new Input{{-1,-1},false};if(pipe2(value->fds,O_NONBLOCK|O_CLOEXEC)){delete value;return -errno;}
        *input=active=value;awake=false;
    }else{
        if(std::strcmp(port,"hw:1,0,0"))return -EINVAL;
        auto writer=new Input{{-1,-1},true};if(pipe2(writer->fds,O_NONBLOCK|O_CLOEXEC)){delete writer;return -errno;}*output=outgoing=writer;
    }
    return 0;
}
int snd_rawmidi_close(Input* input){close(input->fds[0]);close(input->fds[1]);if(input->output)outgoing=nullptr;else active=nullptr;delete input;return 0;}
int snd_rawmidi_poll_descriptors_count(Input*){return 1;}
int snd_rawmidi_poll_descriptors(Input* input,pollfd* descriptors,unsigned count){if(count!=1)return -EINVAL;descriptors[0]={input->fds[0],POLLIN,0};return 1;}
int snd_rawmidi_poll_descriptors_revents(Input*,pollfd* descriptors,unsigned count,unsigned short* events){if(count!=1)return -EINVAL;*events=descriptors[0].revents;return 0;}
ssize_t snd_rawmidi_read(Input* input,void* data,size_t size){const auto result=read(input->fds[0],data,size);return result<0?-errno:result;}
ssize_t snd_rawmidi_write(Input* input,const void* data,size_t size){if(!input->output)return -EINVAL;if(writeError)return writeError;const auto bytes=static_cast<const unsigned char*>(data);sent.insert(sent.end(),bytes,bytes+size);const unsigned char activation[]={0xf0,0x47,0x7f,0x47,0x62,0,1,5,0xf7};if(size==sizeof activation&&std::memcmp(bytes,activation,size)==0)awake=true;return ssize_t(size);}
ssize_t panel_test_send(const void* data,size_t size){return active&&(!std::getenv("MPC_MANAGER_PANEL_FIXTURE")||awake)?write(active->fds[1],data,size):-EPIPE;}
int panel_test_closed(){return active==nullptr&&outgoing==nullptr;}
size_t panel_test_output(void* data,size_t size){if(size<sent.size())return size_t(-1);std::memcpy(data,sent.data(),sent.size());return sent.size();}
void panel_test_reset(int error){sent.clear();writeError=error;}
}
