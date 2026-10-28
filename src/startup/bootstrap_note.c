#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PMU_NOTE_MEDIA
#define PMU_NOTE_MEDIA "/media"
#endif

int main(int argc,char **argv) {
    if(argc!=5)return 1;
    const size_t prefix=strlen(PMU_NOTE_MEDIA);
    if(strncmp(argv[1],PMU_NOTE_MEDIA,prefix)||argv[1][prefix]!='/')return 1;
    const char *name=argv[1]+prefix+1;
    if(!*name||strchr(name,'/')||!strcmp(name,".")||!strcmp(name,".."))return 1;
    if(strspn(argv[2],"0123456789:")!=strlen(argv[2]))return 1;
    uintmax_t identity[4];const char *at=argv[2];char *identity_end;
    for(size_t part=0;part<4;++part){
        errno=0;identity[part]=strtoumax(at,&identity_end,10);
        if(errno||identity_end==at||(part<3?*identity_end!=':':*identity_end!='\0'))return 1;
        at=identity_end+1;
    }
    char *end;errno=0;const unsigned long code=strtoul(argv[3],&end,10);
    if(errno||end==argv[3]||*end||code>255)return 1;
    const int media=open(PMU_NOTE_MEDIA,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(media<0)return 1;
    const int directory=openat(media,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(media);
    if(directory<0)return 1;
    struct stat volume,marker;
    if(fstat(directory,&volume)||identity[0]!=(uintmax_t)volume.st_dev||identity[1]!=(uintmax_t)volume.st_ino){close(directory);return 1;}
    const int marker_fd=openat(directory,".mpc-manager-id",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(marker_fd<0){close(directory);return 1;}
    const int marker_ok=fstat(marker_fd,&marker)==0&&S_ISREG(marker.st_mode)&&marker.st_nlink==1
        &&identity[2]==(uintmax_t)marker.st_dev&&identity[3]==(uintmax_t)marker.st_ino&&marker.st_dev==volume.st_dev;
    close(marker_fd);
    if(!marker_ok){close(directory);return 1;}
    char line[65536],stamp[32];time_t now=time(NULL);struct tm local;
    if(!localtime_r(&now,&local)||!strftime(stamp,sizeof stamp,"%d-%m-%Y %H:%M:%S",&local)){close(directory);return 1;}
    const int length=snprintf(line,sizeof line,"%s helper refused to start (exit %lu): %s\n",stamp,code,argv[4]);
    if(length<=0||(size_t)length>=sizeof line){close(directory);return 1;}
    const int file=openat(directory,"bootstrap-result.txt",O_WRONLY|O_APPEND|O_CREAT|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600);
    if(file<0){close(directory);return 1;}
    int result=1;struct stat status;
    if(flock(file,LOCK_EX|LOCK_NB)==0&&fstat(file,&status)==0&&S_ISREG(status.st_mode)&&status.st_nlink==1
       &&status.st_dev==volume.st_dev&&status.st_size>=0&&status.st_size<=65536
       &&length<=65536-status.st_size){
        ssize_t count;do{count=write(file,line,(size_t)length);}while(count<0&&errno==EINTR);
        if(count==length&&fsync(file)==0)result=0;
    }
    close(file);close(directory);return result;
}
