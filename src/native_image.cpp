#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "native_image.hpp"
#include "checksum.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>

namespace pmu {
namespace {
namespace fs=std::filesystem;
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other)noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;}
};
Fd directory(const fs::path& path){
    need(path.is_absolute()&&path.lexically_normal()==path&&path.native().find('\0')==std::string::npos,"Unsafe native-image directory");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(current.value>=0,"Cannot inspect native-image directory");
    for(const auto& part:path.relative_path()){Fd next(openat(current.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));need(next.value>=0,"Native-image path is missing or contains a link");current=std::move(next);}return current;
}
bool fat(int fd){struct statfs value{};return fstatfs(fd,&value)==0&&(value.f_type==0x2011bab0||value.f_type==0x4d44);}
struct stat regular(int fd,bool anonymous=false){
    struct stat value{};need(fstat(fd,&value)==0&&S_ISREG(value.st_mode)&&value.st_uid==geteuid()&&value.st_nlink==(anonymous?0:1)&&(!(value.st_mode&0022)||(!anonymous&&fat(fd))),"Unsafe native-image file");return value;
}
std::string descriptor(int fd){return "/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);}
void elf(int fd,const std::string& architecture){
    std::array<unsigned char,64> header{};const auto count=pread(fd,header.data(),header.size(),0);const bool wide=architecture=="aarch64";
    auto word=[&](size_t at){return unsigned(header[at])|(unsigned(header[at+1])<<8);};
    need((architecture=="armv7"||wide)&&count>=ssize_t(wide?64:52)&&std::memcmp(header.data(),"\177ELF",4)==0&&header[4]==(wide?2:1)&&header[5]==1&&header[6]==1&&(word(16)==3||word(16)==2)&&word(18)==(wide?183:40)&&word(wide?52:40)==(wide?64:52),"Native image has the wrong architecture or file type");
}
Fd verifiedSource(const NativeImageSpec& image){
    need(image.executable.is_absolute()&&image.executable.lexically_normal()==image.executable&&image.executable.native().find('\0')==std::string::npos&&image.sha256.size()==64&&image.sha256.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid native-image identity");auto parent=directory(image.executable.parent_path());Fd source(openat(parent.value,image.executable.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));need(source.value>=0,"Cannot open native image");
    const auto before=regular(source.value);need(before.st_size>=52&&uint64_t(before.st_size)<=64*1024*1024,"Invalid native-image size");elf(source.value,image.architecture);need(fileSha256(descriptor(source.value))==image.sha256,"Native-image checksum mismatch");const auto after=regular(source.value);need(before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"Native image changed during verification");return source;
}
}
void validateNativeImage(const NativeImageSpec& image){(void)verifiedSource(image);}
PinnedNativeImage::~PinnedNativeImage(){if(value>=0)close(value);}
PinnedNativeImage::PinnedNativeImage(PinnedNativeImage&& other)noexcept:value(other.value){other.value=-1;}
PinnedNativeImage& PinnedNativeImage::operator=(PinnedNativeImage&& other)noexcept{if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;}
PinnedNativeImage pinNativeImage(const NativeImageSpec& image,const fs::path& scratch){
    auto source=verifiedSource(image);const auto before=regular(source.value);auto target=directory(scratch);struct stat folder{};need(fstat(target.value,&folder)==0&&folder.st_uid==geteuid()&&!(folder.st_mode&0022),"Unsafe native-image scratch directory");
    const auto pattern=descriptor(target.value)+"/.native-image-XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);Fd copy(mkostemp(name.data(),O_CLOEXEC));need(copy.value>=0,"Cannot create private native-image copy");
    const auto leaf=fs::path(name.data()).filename().string();if(unlinkat(target.value,leaf.c_str(),0)!=0){unlinkat(target.value,leaf.c_str(),0);throw std::runtime_error("Cannot unlink private native-image copy");}regular(copy.value,true);auto named=directory(scratch);struct stat current{};need(fstat(named.value,&current)==0&&current.st_dev==folder.st_dev&&current.st_ino==folder.st_ino,"Native-image scratch directory was replaced");
    std::array<char,65536> buffer{};uint64_t bytes=0;
    for(;;){const auto n=read(source.value,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;need(n>=0,"Cannot copy native image");if(!n)break;bytes+=uint64_t(n);need(bytes<=uint64_t(before.st_size),"Native image changed while copying");size_t at=0;while(at<size_t(n)){const auto written=write(copy.value,buffer.data()+at,size_t(n)-at);if(written<0&&errno==EINTR)continue;need(written>0,"Cannot write native-image copy");at+=size_t(written);}}
    need(bytes==uint64_t(before.st_size)&&fchmod(copy.value,0500)==0&&fsync(copy.value)==0,"Cannot finish native-image copy");need(fileSha256(descriptor(copy.value))==image.sha256,"Native-image RAM copy differs");Fd result(open(descriptor(copy.value).c_str(),O_RDONLY|O_CLOEXEC));need(result.value>=0,"Cannot pin native-image copy");regular(result.value,true);const int fd=result.value;result.value=-1;return PinnedNativeImage(fd);
}
}
