#include "thumbnail.hpp"
#include <png.h>
#include <cstdio>
#include <jpeglib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

static int checks=0;
static void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);++checks;}
static std::vector<unsigned char> png(int width,int height,bool transparent=false){
    png_image image{};image.version=PNG_IMAGE_VERSION;image.width=width;image.height=height;image.format=PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(size_t(width)*height*4);
    for(size_t i=0;i<pixels.size();i+=4){pixels[i]=240;pixels[i+1]=80;pixels[i+2]=20;pixels[i+3]=transparent?0:255;}
    size_t bytes=0;if(!png_image_write_to_memory(&image,nullptr,&bytes,0,pixels.data(),0,nullptr))throw std::runtime_error("PNG fixture allocation failed");
    std::vector<unsigned char> result(bytes);if(!png_image_write_to_memory(&image,result.data(),&bytes,0,pixels.data(),0,nullptr))throw std::runtime_error("PNG fixture creation failed");result.resize(bytes);return result;
}
static std::vector<unsigned char> jpeg(int width,int height){
    jpeg_compress_struct encoder{};jpeg_error_mgr error{};encoder.err=jpeg_std_error(&error);jpeg_create_compress(&encoder);
    unsigned char* bytes=nullptr;unsigned long length=0;jpeg_mem_dest(&encoder,&bytes,&length);encoder.image_width=width;encoder.image_height=height;encoder.input_components=3;encoder.in_color_space=JCS_RGB;jpeg_set_defaults(&encoder);jpeg_start_compress(&encoder,TRUE);
    std::vector<unsigned char> row(width*3,100);while(encoder.next_scanline<encoder.image_height){JSAMPROW pointer=row.data();jpeg_write_scanlines(&encoder,&pointer,1);}jpeg_finish_compress(&encoder);
    std::vector<unsigned char> result(bytes,bytes+length);jpeg_destroy_compress(&encoder);std::free(bytes);return result;
}
static void rejected(const std::vector<unsigned char>& bytes,const char* label){bool failed=false;try{ThumbnailCache::decode(bytes);}catch(const std::exception&){failed=true;}check(failed,label);}
static void ready(ThumbnailCache& cache,uint64_t previous){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(cache.revision()==previous&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));check(cache.revision()!=previous,"Background thumbnail did not complete");}
int main(){try{
    for(const char* url:{"https://raw.githubusercontent.com/owner/repo/main/screen.png","https://github.com/user-attachments/assets/example"})check(ThumbnailCache::validUrl(url),"Public HTTPS screenshot accepted");
    for(const char* url:{"http://example.com/a.png","file:///tmp/a.png","https://user:password@example.com/a.png","https://127.0.0.1/a.png","https://[::1]/a.png","https://example.com/a\n.png","https://example.com\\evil/a.png","https://example.com:8080/a.png","https:///a.png"})check(!ThumbnailCache::validUrl(url),"Unsafe screenshot URL rejected");
    check(!ThumbnailCache::validUrl("https://example.com/"+std::string(2048,'a')),"Oversized screenshot URL rejected");
    const auto landscape=png(1280,800),portrait=jpeg(100,200);auto image=ThumbnailCache::decode(landscape);
    check(image.width==124&&image.height==78,"Landscape PNG fits inside bordered128x80 without cropping");
    check(image.pixels.front()==0xfff05014&&image.pixels.back()==0xfff05014,"PNG colour is retained during resampling");
    image=ThumbnailCache::decode(portrait);check(image.width==39&&image.height==78,"JPEG portrait preserves aspect ratio");
    image=ThumbnailCache::decode(png(32,32,true));check(std::all_of(image.pixels.begin(),image.pixels.end(),[](uint32_t p){return p==0;}),"Transparent pixels remain transparent without colour fringe");
    check(ThumbnailCache::decode(png(1024,1024)).width==78,"Pixel limit boundary accepted");
    rejected({},"Empty image rejected");rejected(std::vector<unsigned char>(ThumbnailCache::inputLimit+1,0),"Compressed input limit enforced");rejected({'S','V','G'},"Unsupported format rejected");
    auto truncated=landscape;truncated.resize(30);rejected(truncated,"Truncated PNG rejected");
    auto corrupt=landscape;corrupt[29]^=1;rejected(corrupt,"Invalid PNG checksum rejected");
    rejected(png(2049,1),"Dimension limit enforced before output allocation");rejected(png(1025,1024),"Decoded pixel budget enforced");
    truncated=portrait;truncated.resize(20);rejected(truncated,"Truncated JPEG rejected");rejected(jpeg(2049,1),"JPEG dimension limit enforced");rejected(jpeg(1025,1024),"JPEG pixel budget enforced");
    std::atomic<int> calls{0},active{0},maximum{0};
    ThumbnailCache cache([&](const std::string& url,const ThumbnailCache::Cancelled& cancelled){++calls;const int concurrent=++active;maximum=std::max(maximum.load(),concurrent);if(url.find("slow")!=std::string::npos)while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));--active;if(url.find("bad")!=std::string::npos)return std::vector<unsigned char>{0};return landscape;});
    const std::string first="https://example.com/first.png",bad="https://example.com/bad.png";
    auto revision=cache.revision();cache.visible({first,first,"file:///bad"});ready(cache,revision);check(calls==1&&cache.get(first),"Visible requests deduplicate and decode asynchronously");
    revision=cache.revision();cache.visible({first});std::this_thread::sleep_for(std::chrono::milliseconds(20));check(cache.revision()==revision&&calls==1,"Cached artwork never redownloads on redraw");
    revision=cache.revision();cache.visible({bad});ready(cache,revision);check(!cache.get(bad),"Invalid image produces fallback");const auto negativeCalls=calls.load();cache.visible({first});cache.visible({bad});std::this_thread::sleep_for(std::chrono::milliseconds(20));check(calls==negativeCalls,"Failed URLs are negatively cached");
    const auto slow="https://example.com/slow.png";cache.visible({slow});const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!active&&std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(std::chrono::milliseconds(2));check(active==1,"Slow fixture started");revision=cache.revision();const auto start=std::chrono::steady_clock::now();cache.visible({"https://example.com/next.png"});ready(cache,revision);check(std::chrono::steady_clock::now()-start<std::chrono::seconds(1)&&!cache.get(slow),"Page change cancels stale download without blocking UI");
    for(int i=0;i<130;++i){revision=cache.revision();cache.visible({"https://example.com/"+std::to_string(i)+".png"});ready(cache,revision);check(cache.bytes()<=ThumbnailCache::cacheLimit,"Scaled image cache stays bounded");}
    const auto failedCalls=calls.load();cache.visible({bad});std::this_thread::sleep_for(std::chrono::milliseconds(20));check(calls==failedCalls,"Negative cache survives scaled-image LRU eviction");
    check(maximum==1,"Only one download and decoder runs at a time");check(!cache.get(first),"LRU evicts old scaled artwork");cache.clear();check(cache.bytes()==0&&!cache.get(bad),"Exit/eject cleanup purges image and negative caches");
    revision=cache.revision();cache.visible({bad});ready(cache,revision);check(calls>negativeCalls,"A fresh session can retry failed images");
    const auto shutdown=std::chrono::steady_clock::now();{
        std::atomic<bool> started{false};ThumbnailCache stopping([&](const std::string&,const ThumbnailCache::Cancelled& cancelled){started=true;while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));return landscape;});stopping.visible({first});const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!started&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));check(started,"Shutdown fixture starts");
    }check(std::chrono::steady_clock::now()-shutdown<std::chrono::seconds(1),"Shutdown cancels worker and joins promptly");
    std::cout<<"PASS: "<<checks<<" thumbnail URL, PNG/JPEG, bounds, asynchronous navigation, cache and cleanup checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
