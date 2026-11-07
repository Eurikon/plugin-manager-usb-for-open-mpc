#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "thumbnail.hpp"
#include "crypto.hpp"
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
#include <optional>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
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
    std::string etag,lastModified;
};
bool validator(const std::string& value){return value.size()<=1024&&std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32||c>=127;});}
size_t headers(char* input,size_t size,size_t count,void* context){
    if(size&&count>8192/size)return 0;
    const size_t bytes=size*count;auto& state=*static_cast<Download*>(context);
    try{
        std::string line(input,bytes);
        if(line.rfind("HTTP/",0)==0){state.etag.clear();state.lastModified.clear();return bytes;}
        const auto colon=line.find(':');if(colon==std::string::npos)return bytes;
        auto name=line.substr(0,colon);std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return c>='A'&&c<='Z'?c+32:c;});
        auto value=line.substr(colon+1);const auto start=value.find_first_not_of(" \t");value=start==std::string::npos?"":value.substr(start);const auto end=value.find_last_not_of(" \t\r\n");value=end==std::string::npos?"":value.substr(0,end+1);
        if(validator(value)){if(name=="etag")state.etag=value;else if(name=="last-modified")state.lastModified=value;}
        return bytes;
    }catch(...){return 0;}
}
size_t receive(char* input,size_t size,size_t count,void* context){
    auto& state=*static_cast<Download*>(context);
    if(state.cancelled()||size>ThumbnailCache::inputLimit||count>ThumbnailCache::inputLimit/std::max(size,size_t(1)))return 0;
    const size_t bytes=size*count;if(bytes>ThumbnailCache::inputLimit-state.data.size())return 0;
    try{state.data.insert(state.data.end(),input,input+bytes);}catch(...){return 0;}return bytes;
}
int transfer(void* context,curl_off_t,curl_off_t,curl_off_t,curl_off_t){return static_cast<Download*>(context)->cancelled()?1:0;}
ThumbnailCache::Response download(const ThumbnailCache::Request& request,const ThumbnailCache::Cancelled& cancelled){
    static CurlApi api;
    need(validator(request.etag)&&validator(request.lastModified),"Invalid thumbnail change marker");
    std::string conditional=!request.etag.empty()?"If-None-Match: "+request.etag:!request.lastModified.empty()?"If-Modified-Since: "+request.lastModified:"";
    curl_slist header{conditional.data(),nullptr};
    std::unique_ptr<CURL,void(*)(CURL*)> handle(api.init(),api.cleanup);need(bool(handle),"Cannot allocate thumbnail download");
    Download state{{},cancelled,{},{}};state.data.reserve(ThumbnailCache::inputLimit);
    auto set=[&](CURLoption option,auto value){need(api.set(handle.get(),option,value)==CURLE_OK,"Cannot configure thumbnail download");};
    set(CURLOPT_URL,request.url.c_str());set(CURLOPT_NOSIGNAL,1L);set(CURLOPT_CONNECTTIMEOUT_MS,4000L);set(CURLOPT_TIMEOUT_MS,12000L);
    if(!conditional.empty())set(CURLOPT_HTTPHEADER,&header);
    set(CURLOPT_HEADERFUNCTION,headers);set(CURLOPT_HEADERDATA,&state);
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
    need(result==CURLE_OK&&!cancelled(),"Thumbnail download unavailable");
    long status=0;need(api.info(handle.get(),CURLINFO_RESPONSE_CODE,&status)==CURLE_OK,"Thumbnail HTTP response unavailable");
    if(status==304){need(!conditional.empty()&&state.data.empty(),"Unexpected unchanged thumbnail response");return {{},state.etag.empty()?request.etag:state.etag,state.lastModified.empty()?request.lastModified:state.lastModified,true};}
    need(status==200&&!state.data.empty(),"Thumbnail HTTP response unavailable");
    return {std::move(state.data),state.etag,state.lastModified,false};
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
struct ThumbnailCache::Storage {
    struct Record {std::shared_ptr<const Thumbnail> image;std::string etag,lastModified;};
    struct Fd {int value;explicit Fd(int fd):value(fd){}~Fd(){if(value>=0)close(value);}Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;};
    int directory=-1;
    size_t budget;
    uint64_t sequence=0;
    bool reported=false;
    static constexpr size_t entryLimit=48*1024;
    static std::string name(const std::string& url){return "pmu-"+pmu::sha256Text(url)+".thumb";}
    static bool owned(const std::string& value){return value.size()==74&&value.compare(0,4,"pmu-")==0&&value.compare(68,6,".thumb")==0&&value.substr(4,64).find_first_not_of("0123456789abcdef")==std::string::npos;}
    explicit Storage(const std::filesystem::path& path,size_t bytes):budget(std::min(bytes,storageLimit)){
        need(path.is_absolute()&&path.lexically_normal()==path&&path.has_filename()&&path!=path.root_path(),"Invalid thumbnail storage path");
        Fd parent(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));need(parent.value>=0,"Cannot open thumbnail storage root");
        for(const auto& part:path.parent_path().relative_path()){
            const int next=openat(parent.value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(next>=0,"Thumbnail storage parent is missing or linked");close(parent.value);parent.value=next;
        }
        need(mkdirat(parent.value,path.filename().c_str(),0700)==0||errno==EEXIST,"Cannot create thumbnails folder");
        directory=openat(parent.value,path.filename().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);need(directory>=0,"Thumbnails folder is missing or linked");
    }
    ~Storage(){if(directory>=0)close(directory);}
    std::optional<std::string> bytes(const std::string& file){
        Fd input(openat(directory,file.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));if(input.value<0)return {};
        struct stat info{};if(fstat(input.value,&info)||!S_ISREG(info.st_mode)||info.st_nlink!=1||info.st_size<64||uint64_t(info.st_size)>entryLimit)return {};
        std::string value(size_t(info.st_size),'\0');size_t done=0;
        while(done<value.size()){const auto count=read(input.value,value.data()+done,value.size()-done);if(count<0&&errno==EINTR)continue;if(count<=0)return {};done+=size_t(count);}
        char extra;if(read(input.value,&extra,1)!=0)return {};return value;
    }
    std::optional<Record> load(const std::string& url){
        try{
            auto value=bytes(name(url));if(!value)return {};const size_t end=value->size()-64;
            if(pmu::sha256Text(std::string_view(*value).substr(0,end))!=value->substr(end))return {};
            const auto newline=value->find('\n');if(newline==std::string::npos||newline>8192||newline>=end)return {};
            std::istringstream header(value->substr(0,newline));std::string magic,stored,etag,modified;int width=0,height=0;
            if(!(header>>magic>>width>>height>>std::quoted(stored)>>std::quoted(etag)>>std::quoted(modified))||magic!="PMU-THUMBNAIL-LE-1"||stored!=url||width<1||width>ThumbnailCache::width-2||height<1||height>ThumbnailCache::height-2||!validator(etag)||!validator(modified))return {};
            header>>std::ws;if(!header.eof()||end-newline-1!=size_t(width)*height*4)return {};
            auto image=std::make_shared<Thumbnail>();image->width=width;image->height=height;image->pixels.resize(size_t(width)*height);std::memcpy(image->pixels.data(),value->data()+newline+1,image->pixels.size()*4);
            return Record{std::move(image),std::move(etag),std::move(modified)};
        }catch(const std::exception&){return {};}
    }
    void prune(const std::string& keep){
        struct Entry {std::string name;uint64_t size;time_t modified;};std::vector<Entry> files;uint64_t total=0;
        const int duplicate=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(duplicate<0)return;DIR* stream=fdopendir(duplicate);if(!stream){close(duplicate);return;}
        while(auto* item=readdir(stream)){
            if(!owned(item->d_name))continue;
            struct stat info{};
            if(fstatat(directory,item->d_name,&info,AT_SYMLINK_NOFOLLOW)||!S_ISREG(info.st_mode)||info.st_nlink!=1||info.st_size<0||uint64_t(info.st_size)>entryLimit)continue;
            total+=uint64_t(info.st_size);files.push_back({item->d_name,uint64_t(info.st_size),info.st_mtime});if(files.size()>=4096)break;
        }
        closedir(stream);std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.modified<b.modified;});
        for(const auto& file:files){if(total<=budget)break;if(file.name!=keep&&unlinkat(directory,file.name.c_str(),0)==0)total-=file.size;}
    }
    void save(const std::string& url,const Record& record){
        try{
            need(record.image&&validator(record.etag)&&validator(record.lastModified),"Invalid cached thumbnail");
            std::ostringstream header;header<<"PMU-THUMBNAIL-LE-1 "<<record.image->width<<' '<<record.image->height<<' '<<std::quoted(url)<<' '<<std::quoted(record.etag)<<' '<<std::quoted(record.lastModified)<<'\n';
            std::string body=header.str();body.append(reinterpret_cast<const char*>(record.image->pixels.data()),record.image->pixels.size()*4);body+=pmu::sha256Text(body);need(body.size()<=entryLimit&&body.size()<=budget,"Thumbnail storage limit reached");
            const auto target=name(url);const auto before=bytes(target);if(before&&*before==body)return;
            struct stat info{};if(fstatat(directory,target.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0)need(S_ISREG(info.st_mode)&&info.st_nlink==1,"Linked thumbnail cache entry preserved");else need(errno==ENOENT,"Cannot inspect cached thumbnail");
            const auto temporary="."+target+"."+std::to_string(getpid())+"."+std::to_string(++sequence);
            Fd output(openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(output.value>=0,"Cannot stage cached thumbnail");
            try{
                size_t done=0;while(done<body.size()){const auto count=write(output.value,body.data()+done,body.size()-done);if(count<0&&errno==EINTR)continue;need(count>0,"Cannot write cached thumbnail");done+=size_t(count);}
                need(fsync(output.value)==0&&renameat(directory,temporary.c_str(),directory,target.c_str())==0,"Cannot publish cached thumbnail");fsync(directory);prune(target);
            }catch(...){unlinkat(directory,temporary.c_str(),0);throw;}
        }catch(const std::exception& error){if(!reported){reported=true;std::fprintf(stderr,"Thumbnail was not saved: %s\n",error.what());}}
    }
};

ThumbnailCache::ThumbnailCache(Fetch supplied,std::filesystem::path storage,size_t budget):fetch(supplied?std::move(supplied):download),storagePath(std::move(storage)),storageBudget(budget){try{worker=std::thread(&ThumbnailCache::run,this);}catch(const std::system_error&){stopped=true;}}
ThumbnailCache::~ThumbnailCache(){stopped=true;wake.notify_all();if(worker.joinable())worker.join();}
void ThumbnailCache::visible(const std::vector<std::string>& urls){
    std::vector<std::string> filtered;for(const auto& url:urls)if(validUrl(url)&&std::find(filtered.begin(),filtered.end(),url)==filtered.end()&&filtered.size()<6)filtered.push_back(url);
    {std::lock_guard<std::mutex> lock(mutex);if(wanted==filtered)return;wanted=std::move(filtered);}wake.notify_one();
}
std::shared_ptr<const Thumbnail> ThumbnailCache::get(const std::string& url){std::lock_guard<std::mutex> lock(mutex);const auto found=cache.find(url);if(found==cache.end())return {};found->second.used=++clock;return found->second.image;}
size_t ThumbnailCache::bytes() const {std::lock_guard<std::mutex> lock(mutex);return pixelBytes;}
void ThumbnailCache::clear(){std::lock_guard<std::mutex> lock(mutex);++generation;wanted.clear();cache.clear();failed.clear();pixelBytes=0;changed.fetch_add(1);wake.notify_one();}
void ThumbnailCache::run(){
    std::unique_ptr<Storage> storage;
    if(!storagePath.empty())try{storage=std::make_unique<Storage>(storagePath,storageBudget);}catch(const std::exception& error){std::fprintf(stderr,"Thumbnail storage unavailable: %s\n",error.what());}
    std::set<std::string> checked;uint64_t checkedGeneration=0;
    auto publish=[&](const std::string& url,uint64_t epoch,std::shared_ptr<const Thumbnail> image){
        std::lock_guard<std::mutex> lock(mutex);if(stopped||epoch!=generation||std::find(wanted.begin(),wanted.end(),url)==wanted.end())return;
        if(!image){failed.insert(url);changed.fetch_add(1);return;}const size_t size=image->pixels.size()*sizeof(uint32_t);
        const auto existing=cache.find(url);if(existing!=cache.end()){pixelBytes-=existing->second.image->pixels.size()*sizeof(uint32_t);cache.erase(existing);}
        while(!cache.empty()&&(pixelBytes+size>cacheLimit||cache.size()>=256)){auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});pixelBytes-=oldest->second.image->pixels.size()*sizeof(uint32_t);cache.erase(oldest);}
        cache[url]={std::move(image),++clock};pixelBytes+=size;changed.fetch_add(1);
    };
    try{for(;;){
        uint64_t epoch;std::vector<std::string> visible;
        {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopped||generation!=checkedGeneration||(failed.size()<256&&std::any_of(wanted.begin(),wanted.end(),[&](const auto& key){return !failed.count(key)&&(!cache.count(key)||(storage&&!checked.count(key)));}));});if(stopped)return;epoch=generation;visible=wanted;}
        if(epoch!=checkedGeneration){checked.clear();checkedGeneration=epoch;}
        std::map<std::string,Storage::Record> saved;
        if(storage)for(const auto& url:visible){
            bool present;{std::lock_guard<std::mutex> lock(mutex);present=cache.count(url);}
            if(present&&checked.count(url))continue;
            if(auto record=storage->load(url)){if(!present)publish(url,epoch,record->image);saved.emplace(url,std::move(*record));}
        }
        std::string url;
        {std::lock_guard<std::mutex> lock(mutex);if(epoch!=generation||visible!=wanted)continue;for(const auto& key:wanted)if(!failed.count(key)&&(!cache.count(key)||(storage&&!checked.count(key)))){url=key;break;}}
        if(url.empty())continue;
        auto cancelled=[&]{if(stopped)return true;std::lock_guard<std::mutex> lock(mutex);return epoch!=generation||std::find(wanted.begin(),wanted.end(),url)==wanted.end();};
        if(cancelled())continue;
        const auto found=saved.find(url);const auto* prior=found==saved.end()?nullptr:&found->second;
        try{
            const auto response=fetch({url,prior?prior->etag:"",prior?prior->lastModified:""},cancelled);if(cancelled())continue;
            std::shared_ptr<const Thumbnail> image;
            if(response.unchanged){need(prior!=nullptr,"Unchanged thumbnail has no saved image");image=prior->image;}
            else image=std::make_shared<Thumbnail>(decode(response.data));
            const bool same=prior&&prior->image->width==image->width&&prior->image->height==image->height&&prior->image->pixels==image->pixels;
            if(storage)storage->save(url,{image,response.unchanged&&response.etag.empty()?prior->etag:response.etag,response.unchanged&&response.lastModified.empty()?prior->lastModified:response.lastModified});
            if(!same)publish(url,epoch,std::move(image));
        }catch(const std::exception&){if(!cancelled()&&!prior)publish(url,epoch,{});}
        if(!cancelled()){if(checked.size()>=1024)checked.clear();checked.insert(url);}
    }}catch(const std::exception&){stopped=true;changed.fetch_add(1);}
}
