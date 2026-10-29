#include "source_runtime.hpp"
#include "platform.hpp"
#include "version_store.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "metadata.hpp"
#include "signing_fixture.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
void save(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream output(path);output<<value;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write GUI startup fixture");}
int gui(const fs::path& binary,const fs::path& root,const std::string& generation,const std::string& identity,const std::string& mode,const fs::path& font){
    const auto child=fork();if(child<0)throw std::runtime_error("Cannot start GUI fixture");
    if(!child){
        if(setsid()<0)_exit(125);
        const int log=open((root/"manager.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(log<0||dup2(log,1)<0||dup2(log,2)<0)_exit(125);close(log);
        unsetenv("MPC_MANAGER_SD_HELPER");unsetenv("MPC_MANAGER_GENERATION");unsetenv("MPC_MANAGER_SOURCE_ID");
        setenv("SDL_VIDEODRIVER",mode.find("render-failure")!=std::string::npos?"nonexistent-pmu-fixture":"dummy",1);setenv("MPC_MANAGER_UI_TEST","startup-health",1);
        setenv("PATH",((root/"bin").string()+":/usr/bin:/bin").c_str(),1);
        if(mode.rfind("loader-",0)==0){execl(binary.c_str(),binary.c_str(),"--pmu-gui-v1",(root/"source").c_str(),(root/"cache").c_str(),(root/"Expansions").c_str(),(root/"MPC.settings").c_str(),font.c_str(),nullptr);_exit(127);}
        if(mode!="legacy")setenv("MPC_MANAGER_GENERATION",generation.c_str(),1);
        if(mode!="legacy"&&mode!="missing-identity")setenv("MPC_MANAGER_SOURCE_ID",(mode=="wrong-identity"?identity+"1":identity).c_str(),1);
        execl(binary.c_str(),binary.c_str(),(root/"source").c_str(),(root/"cache").c_str(),(root/"Expansions").c_str(),(root/"MPC.settings").c_str(),font.c_str(),nullptr);_exit(127);
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);int status=0;
    while(std::chrono::steady_clock::now()<deadline){const auto done=waitpid(child,&status,WNOHANG);if(done==child)return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);if(done<0&&errno!=EINTR)break;usleep(20000);}
    kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}throw std::runtime_error("GUI startup fixture timed out");
}
}
int main(int argc,char** argv){
    if(argc!=3)return 2;
    const fs::path build=argv[1],evidence=argv[2];char pattern[]="/tmp/pmu-gui-health.XXXXXX";const auto made=mkdtemp(pattern);if(!made)return 1;const fs::path root=made;
    try{
        pmu::test::SigningFixture key;const auto publicKey=key.publicKey();save(root/"key/publisher_key.hpp","#pragma once\nnamespace pmu { inline constexpr char publisherKey[] = \""+publicKey+"\"; }\n");
        const std::string compile="c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"+(root/"key").string()+" -Isrc -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o "+(root/"main.o").string()+" && c++ "+(root/"main.o").string()+" "+build.string()+"/backend.o "+build.string()+"/plugin_storage.o "+build.string()+"/sd_install.o "+build.string()+"/manager_update.o "+build.string()+"/thumbnail.o "+build.string()+"/source_runtime.o "+build.string()+"/version_store.o "+build.string()+"/checksum.o "+build.string()+"/crypto.o "+build.string()+"/native_image.o "+build.string()+"/update_worker.o "+build.string()+"/update_bundle.o "+build.string()+"/update_migration.o "+build.string()+"/runtime_session.o "+build.string()+"/migration_package.o "+build.string()+"/session_handoff.o "+build.string()+"/runtime_handoff.o "+build.string()+"/runtime_worker.o -ldl -o "+(root/"manager").string()+" $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive -Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic";
        if(build.string().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-.")!=std::string::npos||std::system(compile.c_str())!=0)throw std::runtime_error("Cannot build keyed GUI startup fixture");
        if(std::system(("strip "+(root/"manager").string()).c_str())!=0)throw std::runtime_error("Cannot strip GUI startup fixture");
        fs::remove(root/"main.o");
        const std::string architecture=sizeof(void*)==8?"aarch64":"armv7";
        const auto input=root/"input";fs::create_directory(input);fs::copy_file(root/"manager",input/"manager");save(input/"resource","signed resource");
        auto hash=[](const auto& path){return pmu::fileSha256(path);};
        for(const std::string mode:{"valid","legacy","missing-identity","wrong-identity","old-running-image","render-failure","damaged-generation","loader-valid","loader-render-failure"}){
            fs::path fixture=root/mode;if(mode.rfind("loader-",0)==0){char work[]="/tmp/mpc-usb-manager.health.XXXXXX";const auto made=mkdtemp(work);if(!made)throw std::runtime_error("Cannot create GUI loader fixture");fixture=made;}
            const auto source=fixture/"source",location=source/"autorun/runtime/.pmu-versions";for(const auto& folder:{source/"autorun/runtime",fixture/"cache",fixture/"Expansions",fixture/"bin"})fs::create_directories(folder);
            save(source/".mpc-manager-id","mpc-usb-manager-v1\n");save(fixture/"MPC.settings","<PROPERTIES/>\n");save(fixture/"bin/wget","#!/bin/sh\nexit 1\n");chmod((fixture/"bin/wget").c_str(),0700);
            fs::copy_file(root/"manager",input/"manager",fs::copy_options::overwrite_existing);
            pmu::GenerationSpec spec{"1.0.0",architecture,{{"manager",hash(input/"manager")},{"resource",hash(input/"resource")}},{{"manager_"+architecture,"manager"},{"runtime_worker_"+architecture,"manager"}}};
            pmu::VersionStore store(location,geteuid(),hash,{},true,[&](const auto& id,const auto&,const auto& proof){pmu::verifyAuthorization(publicKey,"runtime",id,proof);},managerArchitecture());
            auto stage=[&]{const auto id=pmu::sha256Text(pmu::generationSpecToJson(spec).dump()+'\n');return store.stage(input,spec,key.proof(id,"runtime"));};const auto old=stage();store.seed(old);
            spec.version="2.0.0";if(mode=="old-running-image"){std::ofstream append(input/"manager",std::ios::app);append<<"different incoming image";append.close();spec.files["manager"]=hash(input/"manager");}
            const auto candidate=stage();store.activate(candidate);pmu::SourceRuntime runtime(source,architecture,publicKey);const auto selected=runtime.choose(mode.rfind("loader-",0)!=0);check(selected.generation.id==candidate,"startup selects the pending GUI version");
            if(mode.rfind("loader-",0)==0)save(fixture/"runtime-context.json",nlohmann::json({{"protocol",1},{"source",source.string()},{"helper",""},{"generation",candidate},{"source_identity",selected.identity}}).dump());
            if(mode=="damaged-generation")save(location/candidate/"resource","changed after selection");
            if(mode=="loader-valid")fs::remove(fixture/"cache");
            const auto result=gui(root/"manager",fixture,candidate,selected.identity,mode,build/"font.ttf");
            fs::copy_file(fixture/"manager.log",evidence/(mode+".log"));
            if(mode=="valid"||mode=="loader-valid"){
                check(result==0&&!store.current()->pending,"the actual SDL application confirms its successful first frame");
                check(fs::is_regular_file(fixture/"cache/screen.bmp"),"successful GUI produced its screen capture");fs::copy_file(fixture/"cache/screen.bmp",evidence/(mode=="valid"?"first-frame.bmp":"loader-first-frame.bmp"));
            }else{
                check(result==(mode=="legacy"?0:1),"unhealthy or incomplete startup returns the expected failure");
                check(runtime.choose(true).generation.id==old,"an unconfirmed actual GUI startup restores the verified prior version");
            }
            fs::remove_all(fixture);
        }
        fs::remove_all(root);std::cout<<"PASS: "<<checks<<" gui-health actual first-frame, identity, rendering failure and fallback checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
