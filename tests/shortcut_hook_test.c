#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
typedef struct _snd_rawmidi snd_rawmidi_t;
ssize_t snd_rawmidi_read(snd_rawmidi_t*,void*,size_t);
int main(int argc,char** argv){
    const int chained=argc==2&&!strcmp(argv[1],"--chained");assert(argc==1||chained);
    unsigned (*first_reads)(void)=NULL;unsigned (*second_reads)(void)=NULL;
    if(chained){
        *(void**)&first_reads=dlsym(RTLD_DEFAULT,"shortcut_addin_first_reads");
        *(void**)&second_reads=dlsym(RTLD_DEFAULT,"shortcut_addin_second_reads");
        assert(first_reads&&second_reads&&first_reads()==0&&second_reads()==0);
    }
    int receiver=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);assert(receiver>=0);
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;strcpy(address.sun_path,getenv("MPC_SHORTCUT_SOCKET"));unlink(address.sun_path);
    assert(bind(receiver,(struct sockaddr*)&address,sizeof address)==0);
    unsigned char buffer[16];const unsigned char expected[]={0x90,49,127,0x90,123,127,0x80,49,0,0x80,123,0};
    assert(snd_rawmidi_read((snd_rawmidi_t*)1,buffer,sizeof buffer)==12);
    assert(!memcmp(buffer,expected,sizeof expected)&&errno==EBUSY);
    unsigned char event[4];assert(recv(receiver,event,sizeof event,0)==1&&event[0]=='R');
    const unsigned char notes[]={49,123,49,123};
    for(unsigned i=0;i<4;++i){assert(recv(receiver,event,sizeof event,0)==3&&event[0]=='B'&&event[1]==notes[i]&&event[2]==(i<2));}
    assert(snd_rawmidi_read((snd_rawmidi_t*)1,buffer,1)==1&&buffer[0]==0x90);
    assert(snd_rawmidi_read((snd_rawmidi_t*)1,buffer,0)==-EAGAIN&&errno==EAGAIN);
    assert(snd_rawmidi_read((snd_rawmidi_t*)2,buffer,sizeof buffer)==12);
    assert(recv(receiver,event,sizeof event,0)==-1&&errno==EAGAIN);
    for(unsigned i=0;i<3000;++i)assert(snd_rawmidi_read((snd_rawmidi_t*)1,buffer,sizeof buffer)==12);
    if(chained)assert(first_reads()==3004&&second_reads()==3004);
    close(receiver);unlink(address.sun_path);
    puts(chained?"PASS: existing addins first in exact order, PMU hook last; both RTLD_NEXT chains preserve MIDI/errors and emit shortcut events":"PASS: MIDI bytes, partial reads, return values and errno unchanged; unrelated port readable");
}
