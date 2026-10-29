#include "helper_loader.hpp"
#include "helper_loader_fixture.h"
#include "version_store.hpp"
#include "checksum.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path);output<<text;output.close();if(!output)throw std::runtime_error("Cannot write loader fixture");}
std::string read(const fs::path& path){std::ifstream input(path);if(!input)throw std::runtime_error("Missing loader fixture");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(int argc,char** argv){
    if(argc!=4)return 2;
    std::string pattern="/tmp/pmu-helper-loader.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);const auto made=mkdtemp(name.data());if(!made)return 1;const fs::path root=made;
    try{
#if defined(__aarch64__)
        const std::string architecture="aarch64";
#else
        const std::string architecture="armv7";
#endif
        auto hash=[](const auto& path){return pmu::fileSha256(path);};const auto input=root/"input";fs::create_directory(input);save(root/"preferences.json","keep");
        pmu::VersionStore store(root/"versions",geteuid(),hash);auto spec=[&](int version,const fs::path& library){
            const std::string binary=version==1?"first.so":"changed-layout/native/module.so",script=version==1?"bootstrap.sh":"changed-layout/entry/start.sh";fs::create_directories((input/binary).parent_path());fs::copy_file(library,input/binary,fs::copy_options::overwrite_existing);save(input/script,"#!/bin/sh\nexit 0\n");
            return pmu::GenerationSpec{std::to_string(version)+".0.0",architecture,{{binary,hash(input/binary)},{script,hash(input/script)}},{{"helper_"+architecture,binary},{"bootstrap",script}}};
        };
        const auto first=store.stage(input,spec(1,fs::canonical(argv[1])));store.seed(first);
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&loader.healthy()&&state.version==1&&state.context_valid,"factory helper starts through a verified native module and stable host context");check(loader.generation()==first,"loader reports the selected generation");loader.stop();check(state.stopped==1&&!loader.healthy(),"stopping joins the payload contract before releasing its image");}
        const auto second=store.stage(input,spec(2,fs::canonical(argv[2])));store.activate(second);
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==2&&loader.healthy()&&!store.current()->pending,"changed native helper and bootstrap paths activate only after the payload readiness callback");check(std::string(state.host->bootstrap_script).find("changed-layout/entry/start.sh")!=std::string::npos,"bootstrap path comes from the complete generation roles");}
        store.activate(first);store.boot();store.confirm(first);store.activate(second);
        {PmuLoaderFixture state{0,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&!loader.healthy()&&store.current()->pending,"dlopen and successful start alone cannot confirm startup health");state.host->ready(state.host->userdata);check(loader.healthy()&&!store.current()->pending,"deferred startup health confirms the actual attempted generation");}
        store.activate(first);store.boot();store.confirm(first);store.activate(second);
        {PmuLoaderFixture state{0,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&!loader.healthy(),"unconfirmed helper leaves a durable attempted record");}
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==1&&loader.generation()==first,"next startup automatically restores the last working helper");}
        store.activate(second);
        {PmuLoaderFixture state{1,2,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==1&&loader.generation()==first,"a readiness signal cannot confirm a payload that rejects startup");}
        const auto bad=store.stage(input,spec(3,fs::canonical(argv[3])));store.activate(bad);
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==1&&loader.generation()==first,"incompatible future helper ABI restores the prior helper without an MPC API call");}
        store.activate(second);const auto child=fork();check(child>=0,"fork helper startup interruption fixture");if(!child){PmuLoaderFixture state{0,0,1,0,0,0,nullptr};try{pmu::HelperLoader loader(root/"versions",root,architecture);loader.start(&state);}catch(...){} _exit(90);}
        int status=0;check(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,"payload startup interruption occurs after the durable boot-attempt record");
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==1&&loader.healthy(),"actual native loader recovers after interrupted helper startup");}
        store.activate(second);save(store.current()->directory/"changed-layout/native/module.so","damaged");
        {PmuLoaderFixture state{1,0,0,0,0,0,nullptr};pmu::HelperLoader loader(root/"versions",root,architecture);check(loader.start(&state)&&state.version==1,"corrupt helper is refused before loading and its verified predecessor is used");}
        check(read(root/"preferences.json")=="keep","helper updates and fallback preserve unrelated user data");fs::remove_all(root);std::cout<<"PASS: "<<checks<<" helper-loader native startup, health and fallback checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
