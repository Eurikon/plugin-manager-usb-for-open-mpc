#include "thumbnail.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

static int checks=0;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
static void ready(ThumbnailCache& cache,uint64_t revision){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(cache.revision()==revision&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(5));check(cache.revision()!=revision,"HTTPS transfer did not finish within deadline");}
int main(){try{
    ThumbnailCache cache;
    auto request=[&](const char* path,bool accepted){const std::string url="https://artwork.invalid"+std::string(path);const auto revision=cache.revision();cache.visible({url});ready(cache,revision);check(bool(cache.get(url))==accepted,path);};
    request("/valid.png",true);request("/redirect",true);request("/insecure-redirect",false);request("/redirect-loop",false);request("/oversized-header",false);request("/oversized-stream",false);request("/invalid.png",false);request("/missing",false);request("/untrusted",false);
    cache.visible({"https://artwork.invalid/stall"});std::this_thread::sleep_for(std::chrono::milliseconds(150));const auto revision=cache.revision();const auto start=std::chrono::steady_clock::now();cache.visible({"https://artwork.invalid/after-cancel.png"});ready(cache,revision);check(std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"Cancellation releases stalled HTTPS promptly");check(bool(cache.get("https://artwork.invalid/after-cancel.png")),"Page after cancellation loads normally");
    std::cout<<"PASS: "<<checks<<" offline HTTPS redirect, TLS, size, HTTP error and cancellation checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
