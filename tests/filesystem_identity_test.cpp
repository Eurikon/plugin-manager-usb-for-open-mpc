#include "filesystem_identity.hpp"
#include <iostream>
#include <sys/stat.h>

int main(int argc,char** argv){
    if(argc!=3)return 2;
    const int first=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC),second=open(argv[2],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    try{
        struct stat a{},b{};if(first<0||second<0||fstat(first,&a)||fstat(second,&b))throw std::runtime_error("Cannot open paired mount fixture");if(a.st_dev!=b.st_dev||a.st_ino!=b.st_ino)throw std::runtime_error("Paired mounts must refer to the same filesystem object");if(pmu::mountIdentity(first)==pmu::mountIdentity(second))throw std::runtime_error("Different mounts of the same object must retain distinct identities");close(first);close(second);std::cout<<"PASS: 2 filesystem-identity same-inode mount checks\n";return 0;
    }catch(const std::exception& error){if(first>=0)close(first);if(second>=0)close(second);std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
