#include "shortcut_state.hpp"
#include <dlfcn.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct _snd_rawmidi;
static ssize_t (*nextRead)(_snd_rawmidi*,void*,size_t);
static const char* (*portName)(_snd_rawmidi*);
static pthread_once_t initialized=PTHREAD_ONCE_INIT;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int socketFd=-1;
static sockaddr_un destination{};
static const char* expectedPort;
static PanelMidi parser;
static thread_local uint64_t observedReads;
static void sendEvent(const unsigned char* bytes,size_t size) {
    if(socketFd<0)return;
    if(sendto(socketFd,bytes,size,MSG_DONTWAIT|MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&destination),sizeof destination)<0&&(errno==ENOENT||errno==ECONNREFUSED)){close(socketFd);socketFd=-1;}
}
// Look the next definition up through libc's own dlsym. Another preloaded shim may export its own dlsym (the commander addin
// does), which breaks dlsym(RTLD_NEXT) for every library after it and makes the chain of snd_rawmidi_read wrappers call itself.
static void* nextSymbol(const char* name) {
    typedef void* (*DlsymFunction)(void*,const char*);
    static DlsymFunction libcDlsym=reinterpret_cast<DlsymFunction>(dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.34"));
    if(!libcDlsym)libcDlsym=reinterpret_cast<DlsymFunction>(dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.17"));
    return libcDlsym?libcDlsym(RTLD_NEXT,name):dlsym(RTLD_NEXT,name);
}
static void initialize() {
    *reinterpret_cast<void**>(&nextRead)=nextSymbol("snd_rawmidi_read");
    *reinterpret_cast<void**>(&portName)=nextSymbol("snd_rawmidi_name");
    pthread_mutex_lock(&lock);
    expectedPort=getenv("MPC_PANEL_PORT");const char* path=getenv("MPC_SHORTCUT_SOCKET");
    if(expectedPort&&path&&strlen(path)<sizeof destination.sun_path){
        destination.sun_family=AF_UNIX;strcpy(destination.sun_path,path);
        socketFd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        const unsigned char ready='R';sendEvent(&ready,1);
    }
    pthread_mutex_unlock(&lock);
}
extern "C" uint64_t pmu_shortcut_observed_sequence(const char* path,const char* port) {
    const int saved=errno;
    pthread_mutex_lock(&lock);
    const uint64_t result=expectedPort&&path&&port&&strcmp(path,destination.sun_path)==0
        &&strcmp(port,expectedPort)==0?observedReads:0;
    pthread_mutex_unlock(&lock);
    errno=saved;return result;
}
extern "C" ssize_t snd_rawmidi_read(_snd_rawmidi* port,void* buffer,size_t size) {
    int incoming=errno;pthread_once(&initialized,initialize);errno=incoming;
    if(!nextRead){errno=ENOSYS;return -1;}
    const ssize_t result=nextRead(port,buffer,size);const int saved=errno;
    if(result>0&&portName){
        pthread_mutex_lock(&lock);
        const char* name=portName(port);
        if(socketFd>=0&&name&&strcmp(name,expectedPort)==0){
            ++observedReads;
            for(ssize_t i=0;i<result;++i)parser.feed(static_cast<unsigned char*>(buffer)[i],[](unsigned char note,bool down){const unsigned char message[]={'B',note,static_cast<unsigned char>(down)};sendEvent(message,sizeof message);});
        }
        pthread_mutex_unlock(&lock);
    }
    errno=saved;return result;
}
