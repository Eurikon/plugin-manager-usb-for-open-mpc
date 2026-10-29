#include "thumbnail.hpp"
#include <png.h>
#include <cstdio>
#include <jpeglib.h>
#include <setjmp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#define CURL_DISABLE_DEPRECATION
#include <curl/curl.h>
#include <dlfcn.h>

namespace {
void need(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Pixels {int width=0,height=0;std::vector<unsigned char> rgba;};
struct PngState {
    const unsigned char* data=nullptr;
    size_t size=0,offset=0,allocated=0;
    unsigned char* pixels=nullptr;
    png_bytep* rows=nullptr;
};
struct alignas(std::max_align_t) Allocation {size_t bytes;};
png_voidp pngAllocate(png_structp png,png_alloc_size_t bytes){
    auto* state=static_cast<PngState*>(png_get_mem_ptr(png));
    constexpr size_t limit=24*1024*1024;
    if(bytes>limit||state->allocated>limit-bytes)return nullptr;
    auto* block=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+bytes));
    if(!block)return nullptr;
    block->bytes=bytes;state->allocated+=bytes;return block+1;
}
void pngRelease(png_structp png,png_voidp pointer){if(pointer){auto* block=static_cast<Allocation*>(pointer)-1;static_cast<PngState*>(png_get_mem_ptr(png))->allocated-=block->bytes;std::free(block);}}
void pngRead(png_structp png,png_bytep output,png_size_t count){auto* state=static_cast<PngState*>(png_get_io_ptr(png));if(count>state->size-state->offset)png_error(png,"Truncated thumbnail");std::memcpy(output,state->data+state->offset,count);state->offset+=count;}
void pngFailure(png_structp png,png_const_charp){png_longjmp(png,1);}
void pngWarning(png_structp,png_const_charp){}
Pixels decodePng(const std::vector<unsigned char>& data){
    auto state=std::make_unique<PngState>();state->data=data.data();state->size=data.size();
    png_structp png=png_create_read_struct_2(PNG_LIBPNG_VER_STRING,nullptr,pngFailure,pngWarning,state.get(),pngAllocate,pngRelease);
    need(png,"Cannot allocate PNG decoder");png_infop info=png_create_info_struct(png);
    if(!info){png_destroy_read_struct(&png,nullptr,nullptr);throw std::runtime_error("Cannot allocate PNG metadata");}
    if(setjmp(png_jmpbuf(png))){std::free(state->rows);std::free(state->pixels);png_destroy_read_struct(&png,&info,nullptr);throw std::runtime_error("Invalid PNG thumbnail");}
    png_set_read_fn(png,state.get(),pngRead);png_set_user_limits(png,ThumbnailCache::dimensionLimit,ThumbnailCache::dimensionLimit);
    png_set_chunk_malloc_max(png,ThumbnailCache::inputLimit);png_set_chunk_cache_max(png,8);png_set_keep_unknown_chunks(png,PNG_HANDLE_CHUNK_NEVER,nullptr,0);png_set_crc_action(png,PNG_CRC_ERROR_QUIT,PNG_CRC_ERROR_QUIT);
    png_read_info(png,info);const auto width=png_get_image_width(png,info),height=png_get_image_height(png,info);
    if(!width||!height||size_t(width)*height>ThumbnailCache::pixelLimit)png_error(png,"Thumbnail dimensions exceed limit");
    const int color=png_get_color_type(png,info),depth=png_get_bit_depth(png,info);const bool transparency=png_get_valid(png,info,PNG_INFO_tRNS);
    if(depth==16)png_set_strip_16(png);
    if(color==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);
    if(color==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(png);
    if(transparency)png_set_tRNS_to_alpha(png);
    if(color==PNG_COLOR_TYPE_GRAY||color==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(png);
    if(!(color&PNG_COLOR_MASK_ALPHA)&&!transparency)png_set_filler(png,255,PNG_FILLER_AFTER);
    png_set_interlace_handling(png);png_read_update_info(png,info);
    if(png_get_rowbytes(png,info)!=width*4)png_error(png,"Invalid thumbnail pixel layout");
    state->pixels=static_cast<unsigned char*>(std::malloc(size_t(width)*height*4));state->rows=static_cast<png_bytep*>(std::malloc(height*sizeof(png_bytep)));
    if(!state->pixels||!state->rows)png_error(png,"Cannot allocate thumbnail pixels");
    for(size_t y=0;y<height;++y)state->rows[y]=state->pixels+y*width*4;
    png_read_image(png,state->rows);png_read_end(png,nullptr);png_destroy_read_struct(&png,&info,nullptr);
    Pixels result;result.width=width;result.height=height;
    try{result.rgba.assign(state->pixels,state->pixels+size_t(width)*height*4);}catch(...){std::free(state->rows);std::free(state->pixels);throw;}
    std::free(state->rows);std::free(state->pixels);return result;
}
struct JpegState {
    jpeg_decompress_struct decoder{};
    jpeg_error_mgr errors{};
    jmp_buf jump;
    unsigned char* pixels=nullptr;
    bool created=false;
};
void jpegError(j_common_ptr decoder){auto* state=reinterpret_cast<JpegState*>(decoder);longjmp(state->jump,1);}
void jpegMessage(j_common_ptr decoder,int level){if(level<0)jpegError(decoder);}
Pixels decodeJpeg(const std::vector<unsigned char>& data){
    auto state=std::make_unique<JpegState>();state->decoder.err=jpeg_std_error(&state->errors);state->errors.error_exit=jpegError;state->errors.emit_message=jpegMessage;
    if(setjmp(state->jump)){if(state->created)jpeg_destroy_decompress(&state->decoder);std::free(state->pixels);throw std::runtime_error("Invalid JPEG thumbnail");}
    jpeg_create_decompress(&state->decoder);state->created=true;jpeg_mem_src(&state->decoder,data.data(),data.size());
    if(jpeg_read_header(&state->decoder,TRUE)!=JPEG_HEADER_OK)jpegError(reinterpret_cast<j_common_ptr>(&state->decoder));
    const auto width=state->decoder.image_width,height=state->decoder.image_height;
    if(!width||!height||width>ThumbnailCache::dimensionLimit||height>ThumbnailCache::dimensionLimit||size_t(width)*height>ThumbnailCache::pixelLimit||state->decoder.num_components>3)jpegError(reinterpret_cast<j_common_ptr>(&state->decoder));
    state->decoder.mem->max_memory_to_use=16*1024*1024;state->decoder.out_color_space=JCS_RGB;jpeg_start_decompress(&state->decoder);
    if(state->decoder.output_components!=3)jpegError(reinterpret_cast<j_common_ptr>(&state->decoder));
    state->pixels=static_cast<unsigned char*>(std::malloc(size_t(width)*height*3));if(!state->pixels)jpegError(reinterpret_cast<j_common_ptr>(&state->decoder));
    while(state->decoder.output_scanline<height){JSAMPROW row=state->pixels+size_t(state->decoder.output_scanline)*width*3;if(jpeg_read_scanlines(&state->decoder,&row,1)!=1)jpegError(reinterpret_cast<j_common_ptr>(&state->decoder));}
    jpeg_finish_decompress(&state->decoder);jpeg_destroy_decompress(&state->decoder);state->created=false;
    Pixels result;result.width=width;result.height=height;
    try{result.rgba.resize(size_t(width)*height*4);for(size_t pixel=0;pixel<size_t(width)*height;++pixel){std::memcpy(result.rgba.data()+pixel*4,state->pixels+pixel*3,3);result.rgba[pixel*4+3]=255;}}catch(...){std::free(state->pixels);throw;}
    std::free(state->pixels);return result;
}
struct CurlApi {
    void* library=nullptr;
    CURL* (*init)()=nullptr;
    void (*cleanup)(CURL*)=nullptr;
    CURLcode (*set)(CURL*,CURLoption,...)=nullptr;
    CURLM* (*multiInit)()=nullptr;
    CURLMcode (*multiCleanup)(CURLM*)=nullptr;
    CURLMcode (*add)(CURLM*,CURL*)=nullptr;
    CURLMcode (*remove)(CURLM*,CURL*)=nullptr;
    CURLMcode (*perform)(CURLM*,int*)=nullptr;
    CURLMcode (*wait)(CURLM*,curl_waitfd*,unsigned int,int,int*)=nullptr;
    CURLMsg* (*message)(CURLM*,int*)=nullptr;
    CURLcode (*info)(CURL*,CURLINFO,...)=nullptr;
    CURLcode (*globalInit)(long)=nullptr;
    void (*globalCleanup)()=nullptr;
    template<class T> void symbol(T& target,const char* name){target=reinterpret_cast<T>(dlsym(library,name));need(target,"Firmware thumbnail networking is unavailable");}
    CurlApi(){
        library=dlopen("libcurl.so.4",RTLD_NOW|RTLD_LOCAL);need(library,"Firmware thumbnail networking is unavailable");
        try{symbol(init,"curl_easy_init");symbol(cleanup,"curl_easy_cleanup");symbol(set,"curl_easy_setopt");symbol(multiInit,"curl_multi_init");symbol(multiCleanup,"curl_multi_cleanup");symbol(add,"curl_multi_add_handle");symbol(remove,"curl_multi_remove_handle");symbol(perform,"curl_multi_perform");symbol(wait,"curl_multi_wait");symbol(message,"curl_multi_info_read");symbol(info,"curl_easy_getinfo");symbol(globalInit,"curl_global_init");symbol(globalCleanup,"curl_global_cleanup");need(globalInit(CURL_GLOBAL_DEFAULT)==CURLE_OK,"Cannot initialize thumbnail networking");}catch(...){dlclose(library);throw;}
    }
    ~CurlApi(){globalCleanup();dlclose(library);}
};
struct Download {
    std::vector<unsigned char> data;
    const ThumbnailCache::Cancelled& cancelled;
};
size_t receive(char* input,size_t size,size_t count,void* context){
    auto& state=*static_cast<Download*>(context);
    if(state.cancelled()||size>ThumbnailCache::inputLimit||count>ThumbnailCache::inputLimit/std::max(size,size_t(1)))return 0;
    const size_t bytes=size*count;if(bytes>ThumbnailCache::inputLimit-state.data.size())return 0;
    try{state.data.insert(state.data.end(),input,input+bytes);}catch(...){return 0;}return bytes;
}
int transfer(void* context,curl_off_t,curl_off_t,curl_off_t,curl_off_t){return static_cast<Download*>(context)->cancelled()?1:0;}
std::vector<unsigned char> download(const std::string& url,const ThumbnailCache::Cancelled& cancelled){
    static CurlApi api;
    std::unique_ptr<CURL,void(*)(CURL*)> handle(api.init(),api.cleanup);need(bool(handle),"Cannot allocate thumbnail download");
    Download state{{},cancelled};state.data.reserve(ThumbnailCache::inputLimit);
    auto set=[&](CURLoption option,auto value){need(api.set(handle.get(),option,value)==CURLE_OK,"Cannot configure thumbnail download");};
    set(CURLOPT_URL,url.c_str());set(CURLOPT_NOSIGNAL,1L);set(CURLOPT_CONNECTTIMEOUT_MS,4000L);set(CURLOPT_TIMEOUT_MS,12000L);
    // Numeric protocol masks also work with the stock firmware's older libcurl.
    set(CURLOPT_PROTOCOLS,long(CURLPROTO_HTTPS));set(CURLOPT_REDIR_PROTOCOLS,long(CURLPROTO_HTTPS));set(CURLOPT_FOLLOWLOCATION,1L);set(CURLOPT_MAXREDIRS,3L);
    set(CURLOPT_SSL_VERIFYPEER,1L);set(CURLOPT_SSL_VERIFYHOST,2L);set(CURLOPT_FAILONERROR,1L);set(CURLOPT_NETRC,long(CURL_NETRC_IGNORED));
    set(CURLOPT_MAXFILESIZE_LARGE,curl_off_t(ThumbnailCache::inputLimit));set(CURLOPT_WRITEFUNCTION,receive);set(CURLOPT_WRITEDATA,&state);set(CURLOPT_NOPROGRESS,0L);set(CURLOPT_XFERINFOFUNCTION,transfer);set(CURLOPT_XFERINFODATA,&state);
    std::unique_ptr<CURLM,CURLMcode(*)(CURLM*)> multi(api.multiInit(),api.multiCleanup);need(bool(multi),"Cannot allocate thumbnail connection");
    need(api.add(multi.get(),handle.get())==CURLM_OK,"Cannot start thumbnail connection");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    CURLcode result=CURLE_FAILED_INIT;
    try{
        int running=0;
        do{need(!cancelled()&&std::chrono::steady_clock::now()<deadline,"Thumbnail download cancelled");need(api.perform(multi.get(),&running)==CURLM_OK,"Thumbnail connection failed");if(running){int count=0;const auto start=std::chrono::steady_clock::now();need(api.wait(multi.get(),nullptr,0,40,&count)==CURLM_OK,"Thumbnail connection wait failed");if(!count&&std::chrono::steady_clock::now()-start<std::chrono::milliseconds(1))std::this_thread::sleep_for(std::chrono::milliseconds(10));}}while(running);
        int remaining=0;while(auto* message=api.message(multi.get(),&remaining))if(message->msg==CURLMSG_DONE&&message->easy_handle==handle.get())result=message->data.result;
    }catch(...){api.remove(multi.get(),handle.get());throw;}
    api.remove(multi.get(),handle.get());
    need(result==CURLE_OK&&!cancelled()&&!state.data.empty(),"Thumbnail download unavailable");
    long status=0;need(api.info(handle.get(),CURLINFO_RESPONSE_CODE,&status)==CURLE_OK&&status==200,"Thumbnail HTTP response unavailable");
    return std::move(state.data);
}
}

bool ThumbnailCache::validUrl(const std::string& url){
    if(url.size()>2048||url.rfind("https://",0)!=0)return false;
    for(unsigned char c:url)if(c<=32||c>=127||c=='\\')return false;
    const auto end=url.find_first_of("/?#",8);const auto host=url.substr(8,end==std::string::npos?end:end-8);
    if(host.empty()||host.find('.')==std::string::npos||host.front()=='.'||host.back()=='.'||host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.")!=std::string::npos)return false;
    return host.find_first_not_of("0123456789.")!=std::string::npos;
}
Thumbnail ThumbnailCache::decode(const std::vector<unsigned char>& encoded){
    need(!encoded.empty()&&encoded.size()<=inputLimit,"Thumbnail input exceeds limit");Pixels source;
    if(encoded.size()>=8&&png_sig_cmp(encoded.data(),0,8)==0)source=decodePng(encoded);
    else if(encoded.size()>=3&&encoded[0]==0xff&&encoded[1]==0xd8&&encoded[2]==0xff)source=decodeJpeg(encoded);
    else throw std::runtime_error("Unsupported thumbnail format");
    Thumbnail result;
    constexpr int fittedWidth=width-2,fittedHeight=height-2;
    if(source.width*fittedHeight>=source.height*fittedWidth){result.width=fittedWidth;result.height=std::max(1,source.height*fittedWidth/source.width);}else{result.height=fittedHeight;result.width=std::max(1,source.width*fittedHeight/source.height);}
    result.pixels.resize(size_t(result.width)*result.height);
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x){
        const int x0=x*source.width/result.width,x1=std::max(x0+1,(x+1)*source.width/result.width),y0=y*source.height/result.height,y1=std::max(y0+1,(y+1)*source.height/result.height);
        uint64_t alpha=0,red=0,green=0,blue=0;const unsigned count=(x1-x0)*(y1-y0);
        for(int sy=y0;sy<y1;++sy)for(int sx=x0;sx<x1;++sx){const auto* p=&source.rgba[(size_t(sy)*source.width+sx)*4];alpha+=p[3];red+=unsigned(p[0])*p[3];green+=unsigned(p[1])*p[3];blue+=unsigned(p[2])*p[3];}
        result.pixels[size_t(y)*result.width+x]=uint32_t(alpha/count)<<24|(alpha?uint32_t(red/alpha)<<16|uint32_t(green/alpha)<<8|uint32_t(blue/alpha):0);
    }
    return result;
}
ThumbnailCache::ThumbnailCache(Fetch supplied):fetch(supplied?std::move(supplied):download){try{worker=std::thread(&ThumbnailCache::run,this);}catch(const std::system_error&){stopped=true;}}
ThumbnailCache::~ThumbnailCache(){stopped=true;wake.notify_all();if(worker.joinable())worker.join();}
void ThumbnailCache::visible(const std::vector<std::string>& urls){
    std::vector<std::string> filtered;for(const auto& url:urls)if(validUrl(url)&&std::find(filtered.begin(),filtered.end(),url)==filtered.end()&&filtered.size()<6)filtered.push_back(url);
    {std::lock_guard<std::mutex> lock(mutex);if(wanted==filtered)return;wanted=std::move(filtered);}wake.notify_one();
}
std::shared_ptr<const Thumbnail> ThumbnailCache::get(const std::string& url){std::lock_guard<std::mutex> lock(mutex);const auto found=cache.find(url);if(found==cache.end())return {};found->second.used=++clock;return found->second.image;}
size_t ThumbnailCache::bytes() const {std::lock_guard<std::mutex> lock(mutex);return pixelBytes;}
void ThumbnailCache::clear(){std::lock_guard<std::mutex> lock(mutex);++generation;wanted.clear();cache.clear();failed.clear();pixelBytes=0;changed.fetch_add(1);wake.notify_one();}
void ThumbnailCache::run(){
    try{for(;;){
        std::string url;uint64_t epoch;
        {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopped||(failed.size()<256&&std::any_of(wanted.begin(),wanted.end(),[&](const auto& key){return !cache.count(key)&&!failed.count(key);}));});if(stopped)return;for(const auto& key:wanted)if(!cache.count(key)&&!failed.count(key)){url=key;break;}epoch=generation;}
        auto cancelled=[&]{if(stopped)return true;std::lock_guard<std::mutex> lock(mutex);return epoch!=generation||std::find(wanted.begin(),wanted.end(),url)==wanted.end();};
        std::shared_ptr<const Thumbnail> image;
        try{const auto data=fetch(url,cancelled);if(!cancelled())image=std::make_shared<Thumbnail>(decode(data));}catch(const std::exception&){}
        if(cancelled())continue;
        {std::lock_guard<std::mutex> lock(mutex);if(epoch!=generation)continue;if(!image){failed.insert(url);changed.fetch_add(1);continue;}const size_t size=image->pixels.size()*sizeof(uint32_t);
            while(!cache.empty()&&(pixelBytes+size>cacheLimit||cache.size()>=256)){auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});if(oldest->second.image)pixelBytes-=oldest->second.image->pixels.size()*sizeof(uint32_t);cache.erase(oldest);}
            cache[url]={std::move(image),++clock};pixelBytes+=size;changed.fetch_add(1);
        }
    }}catch(const std::exception&){stopped=true;changed.fetch_add(1);}
}
