#include "thumbnail.hpp"
#include "crypto.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
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
template<class Test> static void until(Test condition){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(!condition()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));check(condition(),"Persistent thumbnail operation did not complete");}
static void persistentChecks(const std::vector<unsigned char>& image,const std::vector<unsigned char>& changed){
    namespace fs=std::filesystem;
    char pattern[]="/tmp/manager-thumbnails.XXXXXX";const auto made=mkdtemp(pattern);if(!made)throw std::runtime_error("Cannot create thumbnail storage fixture");const fs::path root=made,folder=root/"thumbnails";
    const std::string first="https://example.com/persist.png",second="https://example.com/second.png",tag="\"first\"",modified="Wed, 01 Jan 2025 00:00:00 GMT";
    const auto file=[&](const std::string& url){return folder/("pmu-"+pmu::sha256Text(url)+".thumb");};
    const auto content=[](const fs::path& path){std::ifstream input(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{});};
    try{
        std::atomic<int> calls{0};
        {ThumbnailCache cache([&](const ThumbnailCache::Request&,const auto&){++calls;return ThumbnailCache::Response{image,tag,modified};},folder);cache.visible({first,second});until([&]{return cache.get(first)&&cache.get(second);});cache.clear();check(cache.bytes()==0&&fs::is_regular_file(file(first)),"Return clears RAM while retaining the dedicated disk cache");}
        check(calls==2&&fs::is_directory(folder),"Initial images are downloaded once and persisted in their own folder");
        const auto original=content(file(first));const auto timestamp=fs::last_write_time(file(first));std::atomic<bool> matched{false},finished{false};
        {ThumbnailCache cache([&](const ThumbnailCache::Request& request,const auto&){matched=request.etag==tag&&request.lastModified==modified;++calls;finished=true;return ThumbnailCache::Response{{},tag,modified,true};},folder);cache.visible({first});until([&]{return cache.get(first)&&finished;});}
        check(matched&&content(file(first))==original&&fs::last_write_time(file(first))==timestamp,"Unchanged response reuses saved pixels and does not rewrite the image file");
        std::atomic<bool> blocked{false};
        {ThumbnailCache cache([&](const ThumbnailCache::Request&,const ThumbnailCache::Cancelled& cancelled)->ThumbnailCache::Response{blocked=true;while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));throw std::runtime_error("offline");},folder);cache.visible({first,second});until([&]{return blocked&&cache.get(first)&&cache.get(second);});check(cache.get(first)->pixels==ThumbnailCache::decode(image).pixels,"All visible saved images appear before a stalled network check completes");}
        finished=false;
        {ThumbnailCache cache([&](const ThumbnailCache::Request&,const auto&){finished=true;return ThumbnailCache::Response{changed,"\"second\"",modified};},folder);cache.visible({first});until([&]{const auto current=cache.get(first);return finished&&current&&current->pixels==ThumbnailCache::decode(changed).pixels;});check(cache.bytes()==cache.get(first)->pixels.size()*4,"Replacing cached artwork counts its RAM allocation only once");}
        check(content(file(first))!=original,"Changed artwork replaces the saved thumbnail");
        matched=false;
        {ThumbnailCache cache([&](const ThumbnailCache::Request& request,const auto&){matched=request.etag=="\"second\"";return ThumbnailCache::Response{{},request.etag,request.lastModified,true};},folder);cache.visible({first});until([&]{return cache.get(first)&&matched;});}
        {std::ofstream output(file(first),std::ios::binary|std::ios::trunc);output<<"damaged cache";}
        matched=false;
        {ThumbnailCache cache([&](const ThumbnailCache::Request& request,const auto&){matched=request.etag.empty()&&request.lastModified.empty();return ThumbnailCache::Response{image,tag,modified};},folder);cache.visible({first});until([&]{return cache.get(first)&&matched;});}
        check(content(file(first))==original,"Corrupt cached data is refused and rebuilt from a verified image decode");
        fs::create_directory(root/"outside");{std::ofstream output(root/"outside/user.txt");output<<"preserved";}fs::create_directory_symlink(root/"outside",root/"linked");
        {ThumbnailCache cache([&](const auto&,const auto&){return ThumbnailCache::Response{image};},root/"linked");cache.visible({first});until([&]{return bool(cache.get(first));});}
        check(std::distance(fs::directory_iterator(root/"outside"),fs::directory_iterator())==1,"Linked cache directory is not followed or written");
        const std::string linked="https://example.com/linked.png";fs::create_symlink(root/"outside/user.txt",file(linked));
        {ThumbnailCache cache([&](const auto&,const auto&){return ThumbnailCache::Response{image};},folder);cache.visible({linked});until([&]{return bool(cache.get(linked));});}
        check(fs::is_symlink(file(linked))&&content(root/"outside/user.txt")=="preserved","Linked cache entries cannot overwrite another file");
        const auto quota=root/"bounded";
        {ThumbnailCache cache([&](const auto&,const auto&){return ThumbnailCache::Response{image,tag,modified};},quota,80*1024);for(int i=0;i<4;++i){const auto url="https://example.com/quota-"+std::to_string(i)+".png";cache.visible({url});until([&]{return bool(cache.get(url));});}}
        uint64_t bytes=0;for(const auto& entry:fs::directory_iterator(quota))if(entry.is_regular_file())bytes+=entry.file_size();check(bytes<=80*1024,"Persistent thumbnail storage remains bounded");
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
static void detailedChecks(const std::vector<unsigned char>& image){
    namespace fs=std::filesystem;
    char pattern[]="/tmp/manager-detail-image.XXXXXX";const auto made=mkdtemp(pattern);if(!made)throw std::runtime_error("Cannot create image-viewer fixture");const fs::path root=made;
    const std::string url="https://example.com/viewer.png",etag="\"viewer\"";std::atomic<int> bodies{0};std::atomic<bool> conditional{false};
    auto fetch=[&](const ThumbnailCache::Request& request,const auto&){if(request.etag==etag){conditional=true;return ThumbnailCache::Response{{},etag,{},true};}++bodies;return ThumbnailCache::Response{image,etag};};
    try{
        {ThumbnailCache cache(fetch,root);cache.visible({url});until([&]{return bool(cache.get(url));});cache.visible({url},true);until([&]{const auto value=cache.get(url);return value&&value->width>ThumbnailCache::width;});check(bodies==2,"Opening the viewer fetches larger pixels when only a small saved image exists");check(cache.bytes()<=ThumbnailCache::cacheLimit,"Enlarged image replaces the small cache entry within the existing RAM budget");}
        conditional=false;
        {ThumbnailCache cache(fetch,root);cache.visible({url},true);until([&]{const auto value=cache.get(url);return value&&value->width>ThumbnailCache::width&&conditional;});}
        check(bodies==2,"Reopening the viewer reuses saved larger pixels with conditional validation");
        conditional=false;
        {ThumbnailCache cache(fetch,root);cache.visible({url});until([&]{return bool(cache.get(url))&&conditional;});check(cache.get(url)->width<=ThumbnailCache::width,"Cards use a small RAM copy of the saved larger image");cache.visible({url},true);until([&]{return cache.get(url)&&cache.get(url)->width>ThumbnailCache::width;});check(bodies==2,"Opening details restores saved larger pixels without downloading another image");}
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
static void retainedCardChecks(){
    namespace fs=std::filesystem;
    char pattern[]="/tmp/manager-retained-cards.XXXXXX";const auto made=mkdtemp(pattern);if(!made)throw std::runtime_error("Cannot create thumbnail retention fixture");const fs::path root=made;
    const auto image=png(1280,800);const std::string etag="\"retained\"";std::vector<std::string> urls;
    std::atomic<int> bodies{0},validations{0};
    auto fetch=[&](const ThumbnailCache::Request& request,const auto&){if(request.etag==etag){++validations;return ThumbnailCache::Response{{},etag,{},true};}++bodies;return ThumbnailCache::Response{image,etag};};
    const auto file=[&](const std::string& url){return root/("pmu-"+pmu::sha256Text(url)+".thumb");};
    const auto content=[](const fs::path& path){std::ifstream input(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{});};
    try{
        {ThumbnailCache cache(fetch,root,4*1024*1024);for(int i=0;i<12;++i){urls.push_back("https://example.com/retained-"+std::to_string(i)+".png");cache.visible({urls.back()});until([&]{return bool(cache.get(urls.back()));});}}
        std::map<std::string,std::string> original;for(const auto& url:urls)original[url]=content(file(url));
        for(int i=0;i<6;++i){
            const auto url="https://example.com/enlarged-"+std::to_string(i)+".png";urls.push_back(url);
            {ThumbnailCache cache(fetch,root,4*1024*1024);cache.visible({url},true);until([&]{const auto value=cache.get(url);return value&&value->width>ThumbnailCache::width;});}
            uint64_t bytes=0;for(const auto& entry:fs::directory_iterator(root))bytes+=entry.file_size();
            check(bytes<=4*1024*1024,"Enlarged previews remain within the persistent cache budget");
            check(std::all_of(original.begin(),original.end(),[&](const auto& item){return content(file(item.first))==item.second;}),"Opening enlarged previews preserves existing card records byte for byte");
            check(std::all_of(urls.begin(),urls.end(),[&](const auto& key){return fs::is_regular_file(file(key));}),"Older enlarged previews shrink to cards instead of losing catalog artwork");
        }
        const auto downloaded=bodies.load();validations=0;
        {ThumbnailCache cache(fetch,root,4*1024*1024);for(size_t i=0;i<urls.size();++i){cache.visible({urls[i]});until([&]{return bool(cache.get(urls[i]))&&validations>=int(i+1);});}}
        check(bodies==downloaded&&validations==int(urls.size()),"Every retained card reopens using conditional validation without downloading an image body");
        {const auto url="https://example.com/limited-preview.png";ThumbnailCache cache(fetch,root/"limited",80*1024);cache.visible({url},true);until([&]{return bool(cache.get(url));});check(cache.get(url)->width>ThumbnailCache::width,"An oversized persistent preview still displays at full quality in RAM");}
        uint64_t bytes=0;for(const auto& entry:fs::directory_iterator(root/"limited"))bytes+=entry.file_size();check(bytes>0&&bytes<=80*1024,"A preview larger than the disk budget retains a small saved thumbnail");
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
static void crowdedPageChecks(){
    namespace fs=std::filesystem;
    char pattern[]="/tmp/manager-crowded-images.XXXXXX";const auto made=mkdtemp(pattern);if(!made)throw std::runtime_error("Cannot create crowded thumbnail fixture");const fs::path root=made;
    const auto image=png(1700,800);const std::string etag="\"large\"";std::vector<std::string> urls;
    for(int i=0;i<6;++i)urls.push_back("https://example.com/large-"+std::to_string(i)+".png");
    std::atomic<int> bodies{0},validations{0};
    auto fetch=[&](const ThumbnailCache::Request& request,const auto&){if(request.etag==etag){++validations;return ThumbnailCache::Response{{},etag,{},true};}++bodies;return ThumbnailCache::Response{image,etag};};
    auto stored=[&]{std::map<std::string,std::string> result;for(const auto& file:fs::directory_iterator(root)){std::ifstream input(file.path(),std::ios::binary);result[file.path().filename().string()]=std::string(std::istreambuf_iterator<char>(input),{});}return result;};
    try{
        {ThumbnailCache cache(fetch,root);for(const auto& url:urls){cache.visible({url},true);until([&]{const auto value=cache.get(url);return value&&value->width>ThumbnailCache::width;});}}
        const auto before=stored();check(before.size()==6&&bodies==6,"Six enlarged images are saved independently of the RAM limit");
        validations=0;
        {ThumbnailCache cache(fetch,root);auto allSmall=[&]{for(const auto& url:urls){const auto value=cache.get(url);if(!value||value->width>ThumbnailCache::width||value->height>ThumbnailCache::height)return false;}return true;};
            cache.visible(urls);until([&]{return validations==6&&allSmall();});
            const auto revision=cache.revision();std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(allSmall()&&cache.revision()==revision&&validations==6,"A page of enlarged saved images settles without repeated eviction or redraw");
            check(cache.bytes()<=6*size_t(ThumbnailCache::width)*ThumbnailCache::height*4,"Visible cards retain only card-sized RAM allocations");
            cache.visible({});cache.visible(urls);std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(allSmall()&&cache.revision()==revision,"Returning to the same page preserves all six thumbnails without flicker");
            cache.visible({urls.front()},true);until([&]{const auto value=cache.get(urls.front());return value&&value->width>ThumbnailCache::width&&validations>=7;});
            cache.visible(urls);until([&]{return allSmall()&&validations>=13;});
            const auto settled=cache.revision();std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(allSmall()&&cache.revision()==settled&&cache.bytes()<=ThumbnailCache::cacheLimit,"Returning from enlarged artwork restores a stable bounded card cache");
        }
        check(bodies==6&&stored()==before,"Card and enlarged views preserve saved full-size images without downloading or rewriting them");
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
int main(int argc,char** argv){try{
    if(argc>2)throw std::runtime_error("Usage: thumbnail-test [your-image.png]");
    if(argc==2){check(std::filesystem::file_size(argv[1])<=ThumbnailCache::inputLimit,"Supplied image respects the compressed input limit");std::ifstream input(argv[1],std::ios::binary);const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};const auto small=ThumbnailCache::decode(bytes),large=ThumbnailCache::decode(bytes,true);check(!small.pixels.empty()&&!large.pixels.empty(),"Supplied image decodes at both card and viewer sizes");check(large.width>small.width||large.height>small.height,"Supplied image retains more detail in the viewer");}
    for(const char* url:{"https://raw.githubusercontent.com/owner/repo/main/screen.png","https://github.com/user-attachments/assets/example"})check(ThumbnailCache::validUrl(url),"Public HTTPS screenshot accepted");
    for(const char* url:{"http://example.com/a.png","file:///tmp/a.png","https://user:password@example.com/a.png","https://127.0.0.1/a.png","https://[::1]/a.png","https://example.com/a\n.png","https://example.com\\evil/a.png","https://example.com:8080/a.png","https:///a.png"})check(!ThumbnailCache::validUrl(url),"Unsafe screenshot URL rejected");
    check(!ThumbnailCache::validUrl("https://example.com/"+std::string(2048,'a')),"Oversized screenshot URL rejected");
    const auto landscape=png(1280,800),portrait=jpeg(100,200);auto image=ThumbnailCache::decode(landscape);
    check(image.width==124&&image.height==78,"Landscape PNG fits inside bordered128x80 without cropping");
    check(image.pixels.front()==0xfff05014&&image.pixels.back()==0xfff05014,"PNG colour is retained during resampling");
    image=ThumbnailCache::decode(portrait);check(image.width==39&&image.height==78,"JPEG portrait preserves aspect ratio");
    image=ThumbnailCache::decode(png(32,32,true));check(std::all_of(image.pixels.begin(),image.pixels.end(),[](uint32_t p){return p==0;}),"Transparent pixels remain transparent without colour fringe");
    check(ThumbnailCache::decode(png(2048,1024)).width==126,"Pixel limit boundary accepted");
    rejected({},"Empty image rejected");rejected(std::vector<unsigned char>(ThumbnailCache::inputLimit+1,0),"Compressed input limit enforced");rejected({'S','V','G'},"Unsupported format rejected");
    auto truncated=landscape;truncated.resize(30);rejected(truncated,"Truncated PNG rejected");
    auto corrupt=landscape;corrupt[29]^=1;rejected(corrupt,"Invalid PNG checksum rejected");
    rejected(png(2049,1),"Dimension limit enforced before output allocation");rejected(png(2048,1025),"Decoded pixel budget enforced");
    truncated=portrait;truncated.resize(20);rejected(truncated,"Truncated JPEG rejected");rejected(jpeg(2049,1),"JPEG dimension limit enforced");rejected(jpeg(2048,1025),"JPEG pixel budget enforced");
    std::atomic<int> calls{0},active{0},maximum{0};
    ThumbnailCache cache([&](const ThumbnailCache::Request& request,const ThumbnailCache::Cancelled& cancelled){const auto& url=request.url;++calls;const int concurrent=++active;maximum=std::max(maximum.load(),concurrent);if(url.find("slow")!=std::string::npos)while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));--active;if(url.find("bad")!=std::string::npos)return ThumbnailCache::Response{{0}};return ThumbnailCache::Response{landscape};});
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
        std::atomic<bool> started{false};ThumbnailCache stopping([&](const ThumbnailCache::Request&,const ThumbnailCache::Cancelled& cancelled){started=true;while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));return ThumbnailCache::Response{landscape};});stopping.visible({first});const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!started&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));check(started,"Shutdown fixture starts");
    }check(std::chrono::steady_clock::now()-shutdown<std::chrono::seconds(1),"Shutdown cancels worker and joins promptly");
    persistentChecks(landscape,portrait);
    detailedChecks(landscape);
    crowdedPageChecks();
    retainedCardChecks();
    std::cout<<"PASS: "<<checks<<" thumbnail URL, PNG/JPEG, bounds, asynchronous navigation, cache and cleanup checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
