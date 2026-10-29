#include "update_worker.hpp"
#include "checksum.hpp"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--pmu-update-v1"))return 2;
    return pmu::serveUpdateWorker([](const auto& task,const pmu::UpdateProgress& progress){
        const auto mode=task.value("mode",std::string("success"));
        if(mode=="crash")kill(getpid(),SIGKILL);
        if(mode=="timeout")for(;;)pause();
        if(mode=="failure")throw std::runtime_error("expected fixture migration failure");
        if(mode=="bad-progress")progress.fraction(2);
        if(mode=="malformed"){const char text[]="not-json\n";if(write(4,text,sizeof text-1)<0)return nlohmann::json::object();_exit(0);}
        if(mode=="truncated"){const char text[]="{\"incomplete\":";if(write(4,text,sizeof text-1)<0)return nlohmann::json::object();_exit(0);}
        if(mode=="wrong-id"){const std::string text=nlohmann::json{{"protocol",1},{"request_id","wrong-request"},{"kind","result"},{"ok",true},{"result",nlohmann::json::object()}}.dump()+'\n';if(write(4,text.data(),text.size())<0)return nlohmann::json::object();_exit(0);}
        if(mode=="flood")for(int i=0;i<10000;++i)progress.message("progress message");
        progress.message("Applying the complete verified version");progress.fraction(0.5f);progress.fraction(1);
        const int inherited=task.value("inherited_fd",-1);
        return nlohmann::json{{"applied",true},{"message","Update prepared; return to MPC to activate."},{"activation","next_open"},{"echo",task.value("echo",std::string())},{"environment_clean",getenv("LD_PRELOAD")==nullptr},{"running_image_sha256",pmu::runningImageSha256()},{"pinned_image_retained",fcntl(5,F_GETFD)==FD_CLOEXEC&&(fcntl(5,F_GETFL)&O_ACCMODE)==O_RDONLY},{"extra_fd_closed",inherited<0||(fcntl(inherited,F_GETFD)==-1&&errno==EBADF)}};
    });
}
