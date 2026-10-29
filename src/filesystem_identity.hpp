#pragma once
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace pmu {
inline std::string mountIdentity(int directory){
    struct Descriptor {int value;~Descriptor(){if(value>=0)close(value);}} file{open(("/proc/self/fdinfo/"+std::to_string(directory)).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
    if(file.value<0)throw std::runtime_error("Cannot inspect manager storage mount identity");
    std::string text;std::array<char,1024> bytes{};
    for(;;){const auto size=read(file.value,bytes.data(),bytes.size());if(size<0&&errno==EINTR)continue;if(size<0||text.size()+size_t(size)>4096)throw std::runtime_error("Invalid manager storage mount information");if(!size)break;text.append(bytes.data(),size_t(size));}
    std::istringstream lines(text);std::string line,result;
    while(std::getline(lines,line))if(line.rfind("mnt_id:",0)==0){
        if(!result.empty())throw std::runtime_error("Ambiguous manager storage mount identity");
        const auto first=line.find_first_not_of(" \t",7);if(first==std::string::npos)throw std::runtime_error("Missing manager storage mount identity");result=line.substr(first,line.find_last_not_of(" \t")-first+1);if(result.size()>20||result.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Invalid manager storage mount identity");
    }
    if(result.empty())throw std::runtime_error("Manager storage mount identity is unavailable");
    return result;
}
}
