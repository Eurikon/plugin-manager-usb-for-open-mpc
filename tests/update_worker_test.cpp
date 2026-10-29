#include "update_worker.hpp"
#include "checksum.hpp"
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Function> std::string rejects(Function function,const char* message){try{function();}catch(const std::exception& error){++checks;return error.what();}throw std::runtime_error(message);}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    std::string pattern="/tmp/pmu-worker-test.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);const auto made=mkdtemp(name.data());if(!made)return 1;const fs::path root=made;
    try{
#if defined(__aarch64__)
        const std::string architecture="aarch64",other="armv7";
#else
        const std::string architecture="armv7",other="aarch64";
#endif
        const auto program=fs::canonical(argv[1]);pmu::IncomingWorker worker{program,pmu::fileSha256(program),architecture};
        int messages=0,fractions=0;pmu::UpdateProgress progress{[&](const auto& text){check(!text.empty(),"worker progress has a message");++messages;},[&](float fraction){check(fraction>=0&&fraction<=1,"worker fraction is bounded");++fractions;}};
        const auto result=pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"echo","roundtrip"}},progress);
        check(result.at("running_image_sha256")==worker.sha256,"worker self-identification matches its verified executable");
        check(result.at("pinned_image_retained").get<bool>(),"the read-only executable stays pinned without leaking to child commands");
        check(result.at("applied").get<bool>()&&result.at("echo")=="roundtrip"&&result.at("activation")=="next_open","incoming native worker handshake and application succeed");check(messages>0&&fractions>0,"incoming worker supplies UI progress without sharing UI code");check(fs::is_empty(root),"IPC and executable copies leave no named scratch files");
        const char* original=getenv("LD_PRELOAD");const std::string saved=original?original:"";const bool had=original!=nullptr;setenv("LD_PRELOAD","should-not-load",1);
        const auto clean=pmu::runIncomingWorker(worker,root,{{"operation","apply"}});if(had)setenv("LD_PRELOAD",saved.c_str(),1);else unsetenv("LD_PRELOAD");check(clean.at("environment_clean").get<bool>(),"MPC addins do not leak into migration workers");
        const int fd=open("/dev/null",O_RDONLY);check(fd>=0,"extra descriptor fixture opened");const int extra=fcntl(fd,F_DUPFD,100);close(fd);check(extra>=100,"extra descriptor fixture retained");const auto isolated=pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"inherited_fd",extra}});close(extra);check(isolated.at("extra_fd_closed").get<bool>(),"workers do not retain unrelated GUI/device descriptors");
        const auto failure=rejects([&]{pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"mode","failure"}});},"migration error must be returned");check(failure=="expected fixture migration failure","migration failure reaches the user intact");
        for(const std::string mode:{"crash","bad-progress","malformed","truncated","wrong-id"})rejects([&]{pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"mode",mode}});},"unsafe or incomplete worker response is refused");
        const auto started=std::chrono::steady_clock::now();rejects([&]{pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"mode","timeout"}},{},std::chrono::milliseconds(300));},"hung worker must be terminated");check(std::chrono::steady_clock::now()-started<std::chrono::seconds(4),"worker deadline prevents an indefinite update hang");
        check(pmu::runIncomingWorker(worker,root,{{"operation","apply"},{"mode","flood"}}).at("applied").get<bool>(),"progress coalescing preserves space for the final result");
        auto wrong=worker;wrong.architecture=other;rejects([&]{pmu::runIncomingWorker(wrong,root,{{"operation","apply"}});},"worker from the wrong architecture is refused before launch");wrong=worker;wrong.sha256=std::string(64,'a');rejects([&]{pmu::runIncomingWorker(wrong,root,{{"operation","apply"}});},"worker checksum mismatch is refused before launch");
        const auto link=root/"linked-worker";fs::create_symlink(program,link);wrong=worker;wrong.executable=link;rejects([&]{pmu::runIncomingWorker(wrong,root,{{"operation","apply"}});},"linked worker is refused");fs::remove(link);
        const auto legacy=fs::canonical("/bin/true");wrong={legacy,pmu::fileSha256(legacy),architecture};rejects([&]{pmu::runIncomingWorker(wrong,root,{{"operation","apply"}});},"non-protocol native executable is refused by the read-only handshake");
        pmu::Generation generation;generation.directory=program.parent_path();generation.spec.version="1.0.0";generation.spec.architecture=architecture;generation.spec.files={{program.filename().string(),worker.sha256}};generation.spec.entrypoints={{"update_worker_"+architecture,program.filename().string()}};
        check(pmu::generationWorker(generation,architecture).sha256==worker.sha256,"worker is selected by a versioned role rather than a fixed filename");rejects([&]{pmu::generationWorker(generation,other);},"generation architecture mismatch is refused");generation.spec.entrypoints.clear();rejects([&]{pmu::generationWorker(generation,architecture);},"generation must declare its update-worker role");
        check(fs::is_empty(root),"failed updates leave no named worker or IPC artifacts");fs::remove_all(root);std::cout<<"PASS: "<<checks<<" update-worker isolation, integrity and protocol checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
