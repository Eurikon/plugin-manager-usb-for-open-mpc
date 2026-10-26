#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
typedef struct _snd_rawmidi snd_rawmidi_t;
const char* snd_rawmidi_name(snd_rawmidi_t* port){return port==(snd_rawmidi_t*)1?"hw:1,0,1":"hw:1,0,2";}
ssize_t snd_rawmidi_read(snd_rawmidi_t* port,void* buffer,size_t size){
    (void)port;
    const unsigned char bytes[]={0x90,49,127,0x90,123,127,0x80,49,0,0x80,123,0};
    if(size==0){errno=EAGAIN;return -EAGAIN;}
    if(size>sizeof bytes)size=sizeof bytes;
    memcpy(buffer,bytes,size);errno=EBUSY;return (ssize_t)size;
}
