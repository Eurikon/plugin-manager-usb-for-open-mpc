#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
static ssize_t (*next_read)(snd_rawmidi_t*,void*,size_t);
static unsigned reads;

unsigned ADDIN_CALLS(void){return reads;}

ssize_t snd_rawmidi_read(snd_rawmidi_t* port,void* buffer,size_t size){
    const int incoming=errno;
    if(!next_read){
        *(void**)&next_read=dlsym(RTLD_NEXT,"snd_rawmidi_read");
        assert(next_read);
        Dl_info provider;
        assert(dladdr((void*)next_read,&provider)&&provider.dli_fname);
        const char* filename=strrchr(provider.dli_fname,'/');
        assert(!strcmp(filename?filename+1:provider.dli_fname,ADDIN_NEXT));
    }
    ++reads;
    errno=incoming;
    return next_read(port,buffer,size);
}
