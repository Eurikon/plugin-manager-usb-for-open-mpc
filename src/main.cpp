#include "backend.hpp"
#include "sd_install.hpp"
#include "manager_update.hpp"
#include "display.hpp"
#include "plugin_manager_option.hpp"
#ifdef MPC_MANAGER_PLUGIN_STORAGE_FIXTURE
#include "../tests/plugin_storage_fixture.hpp"
#endif
#ifdef MPC_MANAGER_SD_FIXTURE
#include "../tests/sd_install_fixture.hpp"
#include "../tests/manager_update_fixture.hpp"
#endif
#include <SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <future>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <mutex>
#include <sstream>
#include <map>
#include <iomanip>

namespace Theme {
uint32_t background=0xffe9e7e2,panel=0xfff7f6f3,line=0xffcdcac2,ink=0xff1b1c1f,muted=0xff64666d,red=0xffd63a30,amber=0xff7a5600,warning=0xfff5ead1;
constexpr uint32_t white=0xfff4f4f4,lcd=0xff1b1c1f;
void set(bool dark){background=dark?0xff242321:0xffe9e7e2;panel=dark?0xff302e2b:0xfff7f6f3;line=dark?0xff55514b:0xffcdcac2;ink=dark?0xfff4f0e8:0xff1b1c1f;muted=dark?0xffbcb6ad:0xff64666d;red=dark?0xffd94a3f:0xffd63a30;amber=dark?0xffffcc70:0xff7a5600;warning=dark?0xff493c28:0xfff5ead1;}
}
static std::atomic<sig_atomic_t> quitting{0};
static_assert(std::atomic<sig_atomic_t>::is_always_lock_free);
static void stop(int) {quitting=1;}
enum class Icon { None, Grid, Folder, Check, History, Return, Download, Trash, Left, Right, Moon, Sun, Disk, Cog };
struct Screen {
    DisplayGeometry geometry{true};
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* texture=nullptr;
    FT_Library library=nullptr;FT_Face face=nullptr;std::vector<uint32_t> pixels;
    Screen(const std::string& font,const std::string& usb=""):pixels(1280*800,0xff181c22) {
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"0");
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0) throw std::runtime_error(SDL_GetError());
        SDL_DisplayMode mode{};
        if(SDL_GetCurrentDisplayMode(0,&mode)==0&&mode.w>0&&mode.h>0)geometry.portrait=mode.h>mode.w;
        std::string flipNote;
        if(!usb.empty())geometry.flipped=displayFlipFromFile(usb+"/display-rotation.txt",flipNote);
        std::fprintf(stderr,"Display: mode %dx%d, portrait=%d, flipped=%d, devicetree panel-rotation=[%s]%s%s\n",mode.w,mode.h,int(geometry.portrait),int(geometry.flipped),devicetreePanelRotationHex().c_str(),flipNote.empty()?"":"; ",flipNote.c_str());
        window=SDL_CreateWindow("MPC PLUGIN INSTALLER",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,geometry.width(),geometry.height(),SDL_WINDOW_FULLSCREEN_DESKTOP);
        if(!window)throw std::runtime_error(SDL_GetError());
        renderer=SDL_CreateRenderer(window,-1,0);if(!renderer)throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer,geometry.width(),geometry.height());
        texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,1280,800);
        if(!texture||FT_Init_FreeType(&library)||FT_New_Face(library,font.c_str(),0,&face))throw std::runtime_error("Display or font initialization failed");
        FT_Set_Pixel_Sizes(face,0,25);
        SDL_ShowCursor(SDL_DISABLE);
    }
    ~Screen(){if(face)FT_Done_Face(face);if(library)FT_Done_FreeType(library);if(texture)SDL_DestroyTexture(texture);if(renderer)SDL_DestroyRenderer(renderer);if(window)SDL_DestroyWindow(window);SDL_Quit();}
    void rect(int x,int y,int w,int h,uint32_t color){for(int j=std::max(0,y);j<std::min(800,y+h);++j)for(int i=std::max(0,x);i<std::min(1280,x+w);++i)pixels[j*1280+i]=color;}
    void text(int x,int y,const std::string& value,uint32_t color=Theme::ink,int size=25,int width=1200) {
        FT_Set_Pixel_Sizes(face,0,size);const int right=std::min(1252,x+width);
        for(size_t offset=0;offset<value.size();) {
            const unsigned char first=value[offset++];uint32_t code=first;int remaining=0;
            if(first>=0xc2&&first<=0xdf){code=first&31;remaining=1;}
            else if(first>=0xe0&&first<=0xef){code=first&15;remaining=2;}
            else if(first>=0xf0&&first<=0xf4){code=first&7;remaining=3;}
            else if(first>=128)code='?';
            while(remaining--){if(offset>=value.size()||(static_cast<unsigned char>(value[offset])&0xc0)!=0x80){code='?';break;}code=(code<<6)|(static_cast<unsigned char>(value[offset++])&63);}
            if(code<32)code=' ';
            if(FT_Load_Char(face,code,FT_LOAD_RENDER))continue;
            if(x+(face->glyph->advance.x>>6)>right)break;
            const auto& bitmap=face->glyph->bitmap;
            for(unsigned row=0;row<bitmap.rows;++row)for(unsigned col=0;col<bitmap.width;++col){int px=x+face->glyph->bitmap_left+col,py=y-face->glyph->bitmap_top+row;if(px<0||px>=right||py<0||py>=800)continue;unsigned alpha=bitmap.buffer[row*bitmap.pitch+col];auto& dst=pixels[py*1280+px];uint32_t mixed=0xff000000;for(int shift:{0,8,16})mixed|=((((color>>shift)&255)*alpha+((dst>>shift)&255)*(255-alpha))/255)<<shift;dst=mixed;}
            x+=face->glyph->advance.x>>6;
        }
    }
    void box(int x,int y,int w,int h,uint32_t fill,uint32_t border=Theme::line){rect(x,y,w,h,border);rect(x+1,y+1,w-2,h-2,fill);}
    void line(int x,int y,int endX,int endY,uint32_t color) {
        const int dx=std::abs(endX-x),sx=x<endX?1:-1,dy=-std::abs(endY-y),sy=y<endY?1:-1;int error=dx+dy;
        for(;;){rect(x,y,2,2,color);if(x==endX&&y==endY)break;const int twice=2*error;if(twice>=dy){error+=dy;x+=sx;}if(twice<=dx){error+=dx;y+=sy;}}
    }
    void icon(int x,int y,Icon kind,uint32_t color) {
        if(kind==Icon::Moon){for(int row=-11;row<=11;++row)for(int col=-11;col<=11;++col){const int radius=row*row+col*col;if(radius<=121&&(col<0||radius>=81))rect(x+12+col,y+12+row,1,1,color);}}
        else if(kind==Icon::Sun){
            for(int row=-8;row<=8;++row)for(int col=-8;col<=8;++col){const int radius=row*row+col*col;if(radius>=36&&radius<=64)rect(x+12+col,y+12+row,1,1,color);}
            line(x+12,y,x+12,y+2,color);line(x+12,y+22,x+12,y+24,color);line(x,y+12,x+2,y+12,color);line(x+22,y+12,x+24,y+12,color);
            line(x+3,y+3,x+5,y+5,color);line(x+19,y+19,x+21,y+21,color);line(x+3,y+21,x+5,y+19,color);line(x+19,y+5,x+21,y+3,color);
        }
        else if(kind==Icon::Disk){rect(x,y,24,2,color);rect(x,y,2,24,color);rect(x+22,y,2,24,color);rect(x,y+22,24,2,color);rect(x+6,y+2,12,7,color);rect(x+5,y+13,14,2,color);rect(x+5,y+13,2,9,color);rect(x+17,y+13,2,9,color);}
        else if(kind==Icon::Cog){
            const int outline[][2]={{19,9},{22,9},{22,13},{19,13},{18,15},{20,18},{18,20},{15,18},{13,19},{13,22},{9,22},{9,19},{7,18},{4,20},{2,18},{4,15},{3,13},{0,13},{0,9},{3,9},{4,7},{2,4},{4,2},{7,4},{9,3},{9,0},{13,0},{13,3},{15,4},{18,2},{20,4},{18,7}};
            for(int point=0;point<32;++point){const int next=(point+1)%32;line(x+outline[point][0],y+outline[point][1],x+outline[next][0],y+outline[next][1],color);}
            for(int row=7;row<=16;++row)for(int col=7;col<=16;++col){const int dx=col*2-23,dy=row*2-23,radius=dx*dx+dy*dy;if(radius>=36&&radius<=100)rect(x+col,y+row,1,1,color);}
        }
        else if(kind==Icon::Grid){for(int row=0;row<2;++row)for(int col=0;col<2;++col)rect(x+col*13,y+row*13,9,9,color);}
        else if(kind==Icon::Folder){rect(x,y+5,24,2,color);rect(x,y,11,2,color);rect(x,y,2,22,color);rect(x+22,y+5,2,17,color);rect(x,y+20,24,2,color);line(x+10,y,x+15,y+5,color);}
        else if(kind==Icon::Check){line(x,y+12,x+8,y+20,color);line(x+8,y+20,x+23,y+2,color);}
        else if(kind==Icon::History){rect(x+3,y+2,21,2,color);rect(x+22,y+2,2,21,color);rect(x+3,y+21,21,2,color);rect(x+3,y+12,2,10,color);line(x+13,y+6,x+13,y+13,color);line(x+13,y+13,x+18,y+15,color);line(x+2,y+9,x+7,y+4,color);line(x+2,y+9,x,y+3,color);}
        else if(kind==Icon::Download){line(x+12,y,x+12,y+15,color);line(x+5,y+9,x+12,y+16,color);line(x+12,y+16,x+19,y+9,color);rect(x,y+18,2,5,color);rect(x+22,y+18,2,5,color);rect(x,y+21,24,2,color);}
        else if(kind==Icon::Trash){rect(x+2,y+4,21,2,color);rect(x+8,y,10,2,color);rect(x+4,y+7,2,16,color);rect(x+19,y+7,2,16,color);rect(x+4,y+21,17,2,color);rect(x+9,y+9,2,10,color);rect(x+14,y+9,2,10,color);}
        else if(kind==Icon::Left||kind==Icon::Return){line(x+2,y+11,x+11,y+2,color);line(x+2,y+11,x+11,y+20,color);line(x+2,y+11,x+24,y+11,color);}
        else if(kind==Icon::Right){line(x+22,y+11,x+13,y+2,color);line(x+22,y+11,x+13,y+20,color);line(x,y+11,x+22,y+11,color);}
    }
    void button(int x,int y,int w,const std::string& title,bool active=false,bool enabled=true,Icon glyph=Icon::None){box(x,y,w,58,active&&enabled?Theme::red:Theme::panel);const auto color=enabled?(active?Theme::white:Theme::ink):Theme::muted;const int inset=glyph==Icon::None?18:52;if(glyph!=Icon::None)icon(x+16,y+17,glyph,color);text(x+inset,y+38,title,color,25,w-inset-12);}

    void themeButton(int x,int y,int w,bool dark,bool enabled,bool compact=false){
        const Icon glyph=dark?Icon::Sun:Icon::Moon;
        if(!compact){button(x,y,w,dark?"Light":"Dark",false,enabled,glyph);return;}
        box(x,y,w,38,Theme::panel);const auto color=enabled?Theme::ink:Theme::muted;icon(x+12,y+7,glyph,color);text(x+48,y+27,dark?"Light":"Dark",color,23,w-58);
    }
    void badge(int x,int y,int w,const std::string& title,bool warning=false){box(x,y,w,30,warning?Theme::warning:Theme::panel,warning?0xffe0a100:Theme::line);text(x+9,y+22,title,warning?Theme::amber:Theme::muted,19,w-14);}
    void wrapped(int x,int y,const std::string& value,int width,int lines,uint32_t color=Theme::ink,int size=23) {
        FT_Set_Pixel_Sizes(face,0,size);std::istringstream words(value);std::string word,line;int used=0,row=0;
        while(words>>word){int wordWidth=0;for(unsigned char c:word){if(!FT_Load_Char(face,c,FT_LOAD_DEFAULT))wordWidth+=face->glyph->advance.x>>6;}
            if(!line.empty()&&used+wordWidth+size/3>width){text(x,y+row*(size+7),line,color,size,width);if(++row>=lines)return;line.clear();used=0;}
            if(!line.empty()){line+=' ';used+=size/3;}line+=word;used+=wordWidth;
        }
        if(row<lines)text(x,y+row*(size+7),line,color,size,width);
    }
    void present(){SDL_UpdateTexture(texture,nullptr,pixels.data(),1280*4);SDL_RenderClear(renderer);if(geometry.portrait){SDL_Rect destination{-240,240,1280,800};SDL_RenderCopyEx(renderer,texture,nullptr,&destination,geometry.flipped?90:270,nullptr,SDL_FLIP_NONE);}else if(geometry.flipped)SDL_RenderCopyEx(renderer,texture,nullptr,nullptr,180,nullptr,SDL_FLIP_NONE);else SDL_RenderCopy(renderer,texture,nullptr,nullptr);SDL_RenderPresent(renderer);}
};
static std::string storageSize(uint64_t bytes) {
    std::ostringstream value;const bool gigabytes=bytes>=1024ULL*1024*1024;
    value<<std::fixed<<std::setprecision(gigabytes?1:0)<<double(bytes)/(gigabytes?1024.0*1024*1024:1024.0*1024)<<(gigabytes?" GiB":" MiB");return value.str();
}
static int app(int argc,char**argv) {
    if(argc!=6)throw std::runtime_error("Usage: manager USB SESSION SYNTHS SETTINGS FONT");
    PluginStoragePaths storagePaths;
#ifdef MPC_MANAGER_PLUGIN_STORAGE_FIXTURE
    storagePaths=pluginStorageFixture(argv[2],argv[4],std::getenv("MPC_MANAGER_UI_TEST")&&std::string(std::getenv("MPC_MANAGER_UI_TEST"))=="plugin-location-blocked");
#endif
    Manager manager(argv[1],argv[2],argv[3],argv[4],managerArchitecture(),storagePaths);bool dark=false;try{dark=manager.darkTheme();}catch(const std::exception& e){std::fprintf(stderr,"Theme preference: %s\n",e.what());}Theme::set(dark);Screen screen(argv[5],argv[1]);
    auto labelWidth=[&](const std::string& label,int size){int width=0;FT_Set_Pixel_Sizes(screen.face,0,size);for(unsigned char letter:label)if(!FT_Load_Char(screen.face,letter,FT_LOAD_DEFAULT))width+=screen.face->glyph->advance.x>>6;return width;};
    const int setupWidth=12+24+12+labelWidth("SETUP",23)+12,setupX=1252-setupWidth;
    const int themeWidth=16+24+12+std::max(labelWidth("Light",25),labelWidth("Dark",25))+12,themeX=995-themeWidth;
#ifdef MPC_MANAGER_SD_FIXTURE
    SdInstaller sdInstaller=makeSdInstallerFixture(manager.usb,manager.session,std::getenv("MPC_MANAGER_UI_TEST")?std::getenv("MPC_MANAGER_UI_TEST"):"");
    if(std::getenv("MPC_MANAGER_UI_TEST")&&std::string(std::getenv("MPC_MANAGER_UI_TEST"))=="sd-remove-current")manager.usb=sdInstaller.inspect().at("path").get<std::string>();
#else
    SdInstaller sdInstaller(manager.usb,manager.session);
#endif
    const bool sdAvailable=sdInstaller.available();
    bool sdView=false,sdFromWelcome=false,sdReady=false,sdPending=false,sdConfirm=false,sdPicker=false,sdSourceRemoval=false,sourceOnSd=false;
    bool sdSavedFailed=false,sdSavedCompleted=false;int sdPage=0;
    bool pluginLocationView=false,pluginLocationPending=false,pluginBrowsePending=false;std::string pluginBrowsePath;int pluginLocationPage=0;std::string pluginLocationChoice;Json pluginLocationState=Json::object();
    std::string sdSelected,sdOperation,sdSavedStatus;Json sdState=Json::object();std::vector<Json> sdCards;
    bool welcome=true,hideNext=false;try{hideNext=manager.hideWelcome();welcome=!hideNext;}catch(const std::exception& e){std::fprintf(stderr,"Welcome preference: %s\n",e.what());}
    signal(SIGTERM,stop);signal(SIGINT,stop);
    std::mutex progressMutex;std::string progressText;float progressValue=-1;
    auto amount=[&](float value){std::lock_guard<std::mutex> lock(progressMutex);progressValue=value;};
    auto progress=[&](const std::string& text){std::lock_guard<std::mutex> lock(progressMutex);progressText=text;};
    struct Result {std::vector<Json> items;std::string status,catalogError;Json offer=Json::object();Json storage=Json::object();std::vector<Json> cards={};Json managerUpdate=Json::object();Json pluginLocation=Json::object();};
    enum class Work {Open,List,Offer,Preferences,Mutation,SdRead,SdInstall,SdRemove,ManagerCheck,ManagerUpdate,PluginLocationRead,PluginLocationSelect,PluginLocationBrowse};
    std::string view="catalog",status="Loading catalog once for this session...",catalogError;std::vector<Json> items;
    std::map<std::string,int> pages;int selected=-1,page=0;bool confirming=false,busy=true,initial=true,completed=false,failed=false,mutating=false,listReady=false,listPending=false,returning=false,dirty=true,finishFade=false;std::string operation;
    bool managerCheckPending=false,managerCheckFailed=false,managerUpdateConfirm=false,managerUpdated=false;std::string managerUpdateStatus="Not checked";Json managerUpdate=Json::object();
    bool offerPending=false,themePending=false,hidePending=false,savedDark=dark,savedHidden=!welcome,activeDark=dark,activeHidden=!welcome;
    bool saveTheme=false,saveHidden=false;std::string activeView;unsigned long request=0,activeRequest=0;
    Json welcomeOffer={{"label","Install"},{"allowed",false},{"reason","Loading catalog..."}};
    auto currentOffer=[&]() -> Json {for(const auto& item:manager.catalog)if(item.value("id","")==MPC_PM_ID)return manager.installationOffer(item);return {{"label","Unavailable"},{"allowed",false},{"reason","Plugin Manager is unavailable in the current catalog."}};};
    Work work=Work::Open;
    std::future<Result> job=std::async(std::launch::async,[&]{manager.openCatalog();manager.recordInstalled();Result result{{},manager.catalogError.empty()?"Catalog ready. Choose a package.":manager.catalogError,manager.catalogError,currentOffer()};if(manager.arch=="aarch64")result.pluginLocation=manager.pluginStorageInfo();if(sdAvailable){try{result.storage=sdInstaller.inspect();}catch(const std::exception& e){result.storage={{"reason",e.what()}};}}return result;});
    const char* renderView=std::getenv("MPC_MANAGER_RENDER_VIEW");if(renderView)view=renderView;
    auto reload=[&]{++request;listPending=true;listReady=false;items.clear();selected=-1;page=pages[view];confirming=false;dirty=true;};
    Uint32 lastTouch=SDL_GetTicks(),progressEnded=0;bool progressFading=false;
    auto startNext=[&]{
        if(busy||quitting)return;
        if(themePending||hidePending){
            work=Work::Preferences;busy=true;saveTheme=themePending;saveHidden=!saveTheme&&hidePending;activeDark=dark;activeHidden=hideNext;if(saveTheme)themePending=false;if(saveHidden)hidePending=false;
            job=std::async(std::launch::async,[&,theme=saveTheme,hidden=saveHidden,darkValue=activeDark,hideValue=activeHidden]{if(theme)manager.setDarkTheme(darkValue);if(hidden)manager.setHideWelcome(hideValue);return Result{};});
        }else if(pluginBrowsePending&&!returning){
            work=Work::PluginLocationBrowse;busy=true;pluginBrowsePending=false;const auto path=pluginBrowsePath;
            job=std::async(std::launch::async,[&,path]{Result result;result.pluginLocation=manager.pluginStorageFolders(path);return result;});
        }else if(pluginLocationPending&&!returning){
            work=Work::PluginLocationRead;busy=true;pluginLocationPending=false;
            job=std::async(std::launch::async,[&]{Result result;result.pluginLocation=manager.pluginStorageInfo();return result;});
        }else if(sdPending&&!returning){
            work=Work::SdRead;busy=true;sdPending=false;sdReady=false;
            if(sdView)status="Checking SD installation...";
            job=std::async(std::launch::async,[&]{Result result;result.storage=sdInstaller.inspect();result.cards=sdInstaller.targets();return result;});
        }else if(managerCheckPending&&!returning){
            work=Work::ManagerCheck;busy=true;managerCheckPending=false;
            job=std::async(std::launch::async,[&]{Result result;result.managerUpdate=manager.checkManagerUpdate();return result;});
        }else if(offerPending&&!returning){
            work=Work::Offer;busy=mutating=true;offerPending=false;completed=false;failed=false;progressFading=false;amount(-1);
            job=std::async(std::launch::async,[&]{manager.installLatestPlugin(MPC_PM_ID,{MPC_PM_REPO,MPC_PM_TAG_PREFIX,MPC_PM_ASSET_PREFIX},progress,amount);return Result{};});
        }else if(listPending&&!returning){
            work=Work::List;busy=true;activeView=view;activeRequest=request;listPending=false;
            job=std::async(std::launch::async,[&,requestedView=activeView]{return Result{manager.list(requestedView),{}, {},currentOffer()};});
        }
    };
    auto returnToMpc=[&]{returning=true;offerPending=false;sdPending=false;pluginLocationPending=false;pluginBrowsePending=false;managerCheckPending=false;confirming=false;status="Returning to MPC...";dirty=true;};
    auto openSd=[&]{sdFromWelcome=welcome;sdSavedStatus=status;sdSavedFailed=failed;sdSavedCompleted=completed;welcome=false;sdView=true;sdPending=sdAvailable;sdReady=!sdAvailable;sdConfirm=sdPicker=managerUpdateConfirm=false;sdSelected.clear();sdPage=0;offerPending=false;failed=completed=false;progressFading=false;status=sdAvailable?"Checking SD installation...":"Installation and preferences.";dirty=true;};
    auto closeSd=[&]{if(pluginLocationView){if(pluginLocationState.contains("browser")){pluginLocationState.erase("browser");pluginLocationPage=0;dirty=true;return;}pluginLocationView=false;pluginLocationPending=false;dirty=true;return;}if(managerUpdateConfirm){managerUpdateConfirm=false;return;}if(sdConfirm){sdConfirm=false;return;}if(sdPicker){sdPicker=false;return;}sdView=false;welcome=sdFromWelcome;sdPending=false;status=sdSavedStatus;failed=sdSavedFailed;completed=sdSavedCompleted;progressFading=false;dirty=true;};
    const char* testMode=std::getenv("MPC_MANAGER_UI_TEST");
#ifdef MPC_MANAGER_SD_FIXTURE
    const bool sdTest=testMode&&std::string(testMode).rfind("sd-",0)==0;
    const std::string testReleaseVersion=std::getenv("MPC_MANAGER_UI_UPDATE_VERSION")?std::getenv("MPC_MANAGER_UI_UPDATE_VERSION"):"";
#else
    const bool sdTest=false;
#endif
    const bool pluginLocationTest=testMode&&std::string(testMode).rfind("plugin-location",0)==0;
    const bool test=testMode!=nullptr, flow=test&&(std::string(testMode)=="flow"||std::string(testMode)=="touch-flow"),responsive=test&&std::string(testMode)=="responsive",direct=test&&std::string(testMode).rfind("welcome-",0)==0;int frames=0,step=0,welcomeStep=0,presents=0,idlePresents=0;std::string updateTestResult;Uint32 probeStarted=0;const bool startingDark=dark;
    auto click=[&](int x,int y){SDL_Event event{};const auto point=screen.geometry.output(x,y);if(std::string(testMode)=="touch-flow"){event.type=SDL_FINGERUP;int wx,wy,ww,wh;SDL_RenderLogicalToWindow(screen.renderer,float(point.first),float(point.second),&wx,&wy);SDL_GetWindowSize(screen.window,&ww,&wh);event.tfinger.x=float(wx)/ww;event.tfinger.y=float(wy)/wh;SDL_PushEvent(&event);return;}event.type=SDL_MOUSEBUTTONUP;event.button.button=SDL_BUTTON_LEFT;event.button.x=point.first;event.button.y=point.second;SDL_PushEvent(&event);};
    const auto testStarted=SDL_GetTicks();
    auto capture=[&](const char* name){SDL_Surface* surface=SDL_CreateRGBSurfaceWithFormatFrom(screen.pixels.data(),1280,800,32,1280*4,SDL_PIXELFORMAT_ARGB8888);if(!surface)throw std::runtime_error(SDL_GetError());const int result=SDL_SaveBMP(surface,(manager.session/name).c_str());SDL_FreeSurface(surface);if(result!=0)throw std::runtime_error(SDL_GetError());};
    while(!quitting) {
        if(pluginLocationTest&&SDL_GetTicks()-testStarted>15000)throw std::runtime_error("Plugin location UI test timed out at step "+std::to_string(step)+": "+status);
        if(!sdSourceRemoval&&!fs::is_regular_file(manager.usb/".mpc-manager-id")) {status=sourceOnSd?"SD card removed. Returning to MPC.":"USB removed. Returning to MPC.";quitting=1;}
        if(busy&&mutating){std::lock_guard<std::mutex> lock(progressMutex);if(!progressText.empty()){status=progressText;progressText.clear();dirty=true;}}
        if(busy&&job.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
            bool success=true;Result result;
            try{result=job.get();}catch(const std::exception&e){success=false;if(work==Work::ManagerCheck||work==Work::ManagerUpdate){managerUpdateStatus=std::string(work==Work::ManagerCheck?"Check failed: ":"Update failed: ")+e.what();managerCheckFailed=true;if(work==Work::ManagerCheck)managerUpdate=Json::object();else{status=managerUpdateStatus;failed=true;completed=false;}}else if((work!=Work::List||(activeRequest==request&&activeView==view))&&(work!=Work::Offer||(welcome&&!returning))&&(work!=Work::SdRead||sdView)){status=std::string("Failed: ")+e.what();failed=true;completed=false;}}
            if(success&&!result.pluginLocation.empty())pluginLocationState=std::move(result.pluginLocation);
            if(success&&!result.offer.empty())welcomeOffer=std::move(result.offer);
            if(success&&!result.storage.empty()){sdState=std::move(result.storage);sourceOnSd=sdState.value("current",false);}
            busy=false;dirty=true;lastTouch=SDL_GetTicks();
            if(work==Work::Open){initial=false;if(success){status=result.status;catalogError=result.catalogError;failed=false;}reload();}
            else if(work==Work::Preferences){
                if(success){if(saveTheme)savedDark=activeDark;if(saveHidden)savedHidden=activeHidden;}
                else {if(saveTheme&&!themePending){dark=savedDark;Theme::set(dark);}if(saveHidden&&!hidePending)hideNext=savedHidden;}
            }else if(work==Work::PluginLocationRead||work==Work::PluginLocationSelect||work==Work::PluginLocationBrowse){
                if(work==Work::PluginLocationSelect)mutating=false;
                if(success){failed=false;status=work==Work::PluginLocationSelect?"Plugin installation location selected for this session. No plugin files changed.":"Choose Automatic or a plugin location below. The Plugin Manager installation is separate.";if(work==Work::PluginLocationSelect)reload();}
            }else if(work==Work::ManagerCheck){
                if(success){managerUpdate=std::move(result.managerUpdate);managerUpdateStatus=managerUpdate.at("message").get<std::string>();managerCheckFailed=false;}
            }else if(work==Work::ManagerUpdate){
                mutating=false;managerUpdateConfirm=false;
                if(success){managerUpdated=true;managerCheckPending=false;managerCheckFailed=false;completed=true;failed=false;managerUpdateStatus=result.managerUpdate.at("message").get<std::string>();status=managerUpdateStatus;sourceOnSd=result.managerUpdate.value("source_on_sd",sourceOnSd);}
                progressEnded=SDL_GetTicks();progressFading=true;
            }else if(work==Work::Offer){
                mutating=false;if(success){status=MPC_PM_NAME " installed. Return to MPC to use it on a plugin track.";completed=true;failed=false;welcomeOffer={{"label","Installed"},{"allowed",false},{"reason","Catalog release installed."}};}
                progressEnded=SDL_GetTicks();progressFading=true;reload();
            }else if(work==Work::SdRead){
                sdReady=success;if(success){sdCards=std::move(result.cards);sdPage=std::clamp(sdPage,0,std::max(0,(int(sdCards.size())-1)/4));if(sdView)status=sdState.value("configured",false)?sdState.value("reason",""):sdCards.empty()?"No suitable SD card found. Insert your card and choose Refresh.":"Select an SD card. Installation starts only after confirmation.";}
            }else if(work==Work::SdInstall||work==Work::SdRemove){
                mutating=false;sdConfirm=sdPicker=false;
                if(success){completed=true;failed=false;sdReady=true;status=work==Work::SdInstall?(sourceOnSd?"Updated on SD. Return to MPC; keep the SD card inserted.":"Installed on SD. Return to MPC and safely eject the USB. Open the manager by holding SHIFT + MENU for two seconds."):"Manager removed from SD. Plugins, history, preferences and local packages were kept.";if(work==Work::SdRemove&&sdSourceRemoval)returnToMpc();}
                else sdSourceRemoval=false;
                progressEnded=SDL_GetTicks();progressFading=true;
            }else if(work==Work::List&&activeRequest==request&&activeView==view){
                if(success){items=std::move(result.items);listReady=true;page=std::clamp(pages[view],0,std::max(0,(int(items.size())-1)/6));pages[view]=page;if(status=="Loading plugins...")status=view=="catalog"?(catalogError.empty()?"Catalog ready. Choose a package.":catalogError):view=="local"?"Local packages ready. Choose a package.":view=="installed"?"Installed plugins ready. Choose a plugin.":"Installation history ready. Choose a plugin.";}
                if(finishFade){progressEnded=SDL_GetTicks();progressFading=true;finishFade=false;}
            }else if(work==Work::Mutation){mutating=false;if(success){status=operation=="Uninstall"?"Plugin uninstalled.":"Plugin installed. Ready for another selection.";completed=true;failed=false;}finishFade=true;reload();lastTouch=SDL_GetTicks();}
            if(returning)status="Returning to MPC...";
        }
        startNext();
        if(returning&&!busy&&!themePending&&!hidePending)quitting=1;
        const Uint32 fadeElapsed=SDL_GetTicks()-progressEnded;if(progressFading&&fadeElapsed>=600){progressFading=false;dirty=true;}
        if(SDL_GetTicks()-lastTouch>=300000&&!busy)quitting=1;
        if(dirty||progressFading||(!welcome&&busy)||(welcome&&mutating)){
        screen.rect(0,0,1280,800,Theme::background);
        if(sdView){
            screen.text(28,43,"PLUGIN MANAGER SETUP",Theme::ink,31);
            screen.rect(28,55,1224,1,Theme::line);
            screen.button(28,65,180,"Back",false,!mutating,Icon::Return);
            if(sdAvailable||pluginLocationView)screen.button(220,65,180,"Refresh",false,!busy&&!sdConfirm&&!managerUpdateConfirm,Icon::History);
            if(manager.arch=="aarch64"&&!pluginLocationView)screen.button(412,65,300,"Plugin location",false,!busy&&!sdConfirm&&!sdPicker&&!managerUpdateConfirm,Icon::Folder);
            screen.themeButton(themeX,65,themeWidth,dark,!mutating);
            screen.button(1007,65,245,"Return to MPC",false,!mutating,Icon::Return);
            if(pluginLocationView){
                const auto browser=pluginLocationState.value("browser",Json::object());const bool browsing=!browser.empty();
                screen.text(28,159,"Plugin installation location",Theme::ink,30);
                screen.box(28,185,1224,124,Theme::panel);
                screen.text(52,223,browsing?"Choose your plugin folder":pluginLocationState.value("automatic",true)?"Automatic: MPC content download drive":"Manually selected for this session",Theme::ink,26,820);
                screen.text(52,263,browsing?browser.value("path",""):pluginLocationState.value("path","").empty()?"No usable location selected":pluginLocationState.value("path",""),Theme::muted,23,820);
                if(browsing)screen.button(918,207,310,"Parent folder",false,!busy&&!browser.value("parent","").empty(),Icon::Left);
                else screen.button(918,207,310,"Automatic",pluginLocationState.value("automatic",true),!busy&&pluginLocationState.contains("native"),Icon::Check);
                screen.wrapped(28,342,browsing?"Open a folder, then choose Use this folder. It will be registered when you confirm a plugin installation.":pluginLocationState.value("native_error","").empty()?"Select a plugin location below, or select a drive and choose Browse folders. Selection does not install anything.":pluginLocationState.value("native_error",""),1200,2,Theme::muted,22);
                const auto targets=browsing?browser.value("folders",Json::array()):pluginLocationState.value("targets",Json::array());
                for(int row=0;row<3;++row){const int index=pluginLocationPage*3+row;if(index>=int(targets.size()))break;const int y=394+row*64;const auto path=browsing?targets[index].get<std::string>():targets[index].at("path").get<std::string>();const bool chosen=pluginLocationChoice==path;screen.box(28,y,1224,56,Theme::panel,chosen?Theme::red:Theme::line);screen.icon(46,y+16,Icon::Folder,chosen?Theme::red:Theme::muted);screen.text(90,y+36,browsing?fs::path(path).filename().string():path,Theme::ink,25,1110);}
                if(targets.empty()&&!busy)screen.text(28,427,browsing?"No subfolders here.":"No writable, executable plugin drive is available.",Theme::muted,25);
                screen.button(28,605,180,"Previous",false,!busy&&pluginLocationPage>0,Icon::Left);screen.button(220,605,180,"Next",false,!busy&&(pluginLocationPage+1)*3<int(targets.size()),Icon::Right);
                if(!browsing)screen.button(412,605,300,"Browse folders",false,!busy&&!pluginLocationChoice.empty(),Icon::Folder);
                screen.button(942,605,310,browsing?"Use this folder":"Use selected",true,!busy&&(browsing?browser.value("selectable",false):!pluginLocationChoice.empty()),Icon::Check);
            }else if(managerUpdateConfirm){
                screen.text(28,159,"Confirm manager update",Theme::muted,25);
                screen.box(28,185,1224,390,Theme::panel);
                screen.text(52,239,"Download and install Plugin Manager v"+managerUpdate.value("latest_version","")+"?",Theme::ink,34,1150);
                screen.wrapped(52,292,"The verified release updates this manager and its connected managed media. Keep the storage connected until the update finishes.",1150,2,Theme::muted,25);
                screen.wrapped(52,369,"Your plugins, history, preferences and local packages stay in place. Return to MPC after updating; reopen the manager to use the new version.",1150,2,Theme::muted,23);
                screen.button(52,455,340,"Download & install",true,!busy,Icon::Download);
                screen.button(410,455,220,"Cancel",false,!mutating);
            }else if(sdConfirm){
                const bool remove=sdOperation=="Remove";std::string label=sdState.value("label","");
                for(const auto& card:sdCards)if(card.value("uuid","")==sdSelected)label=card.value("label",card.value("path","SD card"));
                screen.text(28,159,remove?"Remove the SD installation":"Confirm installation",Theme::muted,25);
                screen.box(28,185,1224,390,Theme::panel);
                screen.text(52,239,remove?"Remove this manager from SD?":"Install this manager on "+label+"?",Theme::ink,34,1150);
                screen.wrapped(52,292,remove?"Your plugins, installation history, preferences and local packages stay on the card.":sourceOnSd?"After updating, return to MPC. Keep the SD card inserted.":"After installation, return to MPC and safely eject the USB. Keep the SD card inserted; hold SHIFT + MENU for two seconds to open the manager.",1150,2,Theme::muted,25);
                screen.text(52,369,remove?"Plugin Manager folder: Plugin Manager USB":sourceOnSd?"The current session remains available until you return to MPC.":"This USB session remains available until you return to MPC.",Theme::muted,23,1150);
                screen.button(52,425,310,remove?"Confirm removal":"Confirm install",true,!busy,remove?Icon::Trash:Icon::Download);
                screen.button(380,425,220,"Cancel",false,!mutating);
            }else if(sdPicker){
                screen.text(28,159,"Choose the SD card for this manager",Theme::muted,25);
                if(!sdReady)screen.wrapped(28,230,failed?status:"Checking available SD cards...",1150,3,failed?Theme::amber:Theme::muted,27);
                else if(sdCards.empty())screen.wrapped(28,230,"No suitable SD card found. Insert your card and choose Refresh.",1150,3,Theme::muted,27);
                if(sdReady)for(int row=0;row<4;++row){const int index=sdPage*4+row;if(index>=int(sdCards.size()))break;const auto& card=sdCards[index];const int y=185+row*98;const bool chosen=sdSelected==card.value("uuid","");screen.box(28,y,1224,90,Theme::panel,chosen?Theme::red:Theme::line);if(chosen)screen.rect(28,y,4,90,Theme::red);screen.icon(48,y+17,Icon::Disk,chosen?Theme::red:Theme::muted);screen.text(90,y+35,card.value("label",card.value("path","SD card")),Theme::ink,30,850);screen.text(90,y+68,"Plugin Manager folder: Plugin Manager USB",Theme::muted,22,850);screen.text(960,y+52,storageSize(card.value("free_bytes",uint64_t(0)))+" free",Theme::muted,23,270);}
                screen.button(28,605,180,"Previous",false,!busy&&sdReady&&sdPage>0,Icon::Left);screen.button(220,605,180,"Next",false,!busy&&sdReady&&(sdPage+1)*4<int(sdCards.size()),Icon::Right);
                screen.button(942,605,310,"Install to SD",true,!busy&&sdReady&&!sdSelected.empty(),Icon::Download);
            }else{
                const bool installed=sdState.value("configured",false)||sdState.value("can_remove",false);
                screen.text(28,159,"Installation and preferences",Theme::muted,25);
                screen.box(28,185,1224,170,Theme::panel);screen.icon(52,209,Icon::Disk,Theme::red);screen.text(96,235,"Plugin Manager installation",Theme::ink,28,790);
                std::string detail=!sdAvailable?"SD installation requires the supported MPC OS 3.10 helper.":!sdReady?(failed?status:"Checking SD installation..."):installed?sdState.value("installed",false)?"Installed on "+sdState.value("label","SD card"):sdState.value("reason","SD installation unavailable."):sdCards.empty()?"No suitable SD card found. Insert your card and choose Refresh.":"Not installed. Choose an SD card to install the manager.";
                screen.wrapped(96,273,detail,790,2,Theme::muted,23);
                if(sdAvailable&&sdReady){
                    screen.text(96,334,"Plugin Manager folder: Plugin Manager USB",Theme::muted,23,790);
                    screen.button(918,207,310,installed?(sdState.value("installed",false)?"Update manager":"Reinstall manager"):"Install to SD",true,!busy&&(!installed||(sdState.value("can_remove",false)&&!sdState.value("removing",false))),Icon::Download);
                    if(installed)screen.button(918,277,310,"Uninstall manager",false,!busy&&sdState.value("can_remove",false),Icon::Trash);
                }
                screen.box(28,371,1224,115,Theme::panel);screen.icon(52,395,Icon::Grid,Theme::red);screen.text(96,411,"Welcome message",Theme::ink,28,790);
                screen.text(96,447,"Show the welcome screen when the manager opens.",Theme::muted,23,790);
                screen.box(918,414,30,30,!hideNext?Theme::red:Theme::background);if(!hideNext)screen.icon(921,417,Icon::Check,Theme::white);screen.text(964,438,"Show on opening",Theme::ink,25,264);
                screen.box(28,502,1224,150,Theme::panel);screen.icon(52,526,Icon::History,Theme::red);screen.text(96,542,"Plugin Manager updates",Theme::ink,28,790);
                screen.text(96,576,"Check GitHub Releases for a newer manager version.",Theme::muted,23,790);
                screen.wrapped(96,610,managerUpdateStatus,790,2,managerCheckFailed?Theme::amber:Theme::muted,22);
                screen.button(918,514,310,"Check for updates",false,!managerUpdated&&!managerCheckPending&&!(busy&&work==Work::ManagerCheck),Icon::History);
                if(!managerUpdated&&managerUpdate.value("update_available",false)&&managerUpdate.value("downloadable",false))screen.button(918,584,310,"Install update",true,!busy&&!managerCheckPending,Icon::Download);
            }
            screen.box(28,682,1224,98,Theme::lcd,Theme::lcd);
            screen.wrapped(46,713,status,1180,2,failed?0xffffbd91:Theme::white,23);
        }else if(welcome){
            screen.text(64,79,"MPC PLUGIN INSTALLER",Theme::ink,38);
            screen.themeButton(1084,48,132,dark,!mutating,true);
            screen.box(64,115,1152,503,Theme::panel);
            screen.icon(92,126,Icon::Disk,Theme::red);screen.text(134,150,"Your MPC session",Theme::ink,26);
            screen.wrapped(134,184,"Save your project before opening the installer. The MPC application stops while the installer is open and restarts when you return.",1020,2,Theme::muted,22);
            screen.icon(92,222,Icon::Download,Theme::red);screen.text(134,246,"Catalog and local plugins",Theme::ink,26);
            screen.wrapped(134,280,std::string("The catalog loads once per opening. Local ZIP packages on this ")+(sourceOnSd?"SD card":"USB")+" work offline. Catalog data and downloads are cleared when you leave.",1020,2,Theme::muted,22);
            screen.icon(92,318,Icon::Return,Theme::red);screen.text(134,342,"Come back whenever you need",Theme::ink,26);
            screen.wrapped(134,376,std::string("Keep the ")+(sourceOnSd?"SD card":"USB")+" inserted. From MPC, hold SHIFT + MENU for two seconds to reopen the installer.",1020,2,Theme::muted,22);
            screen.rect(92,429,1096,1,Theme::line);
            screen.icon(92,435,Icon::Disk,Theme::red);screen.text(134,459,"Plugin Manager on SD",Theme::ink,26);
            screen.wrapped(134,493,sourceOnSd?"Update or remove the manager installed on your SD card.":"Install this manager on SD and set your opening preferences.",790,1,Theme::muted,22);
            screen.button(958,446,230,"Setup",false,!mutating,Icon::Right);
            screen.rect(92,521,1096,1,Theme::line);
            screen.icon(92,527,Icon::Grid,Theme::red);screen.text(134,551,"Or install \"" MPC_PM_NAME "\" instead",Theme::ink,26);
            screen.wrapped(134,585,"Manage catalog plugins from a plugin track in MPC.",790,1,Theme::muted,22);
            screen.button(958,538,230,welcomeOffer.value("label","Install"),false,!offerPending&&!mutating&&welcomeOffer.value("allowed",false),Icon::Right);
            if(!mutating&&!welcomeOffer.value("allowed",false)&&(welcomeOffer.value("label","")=="Installed"||welcomeOffer.value("label","")=="Unavailable"))screen.text(134,610,welcomeOffer.value("reason",""),Theme::muted,19,790);
            screen.box(64,634,30,30,hideNext?Theme::red:Theme::panel);if(hideNext)screen.icon(67,637,Icon::Check,Theme::white);
            screen.text(110,658,"Hide message",mutating?Theme::muted:Theme::ink,25);
            screen.button(64,680,270,"Return to MPC",false,!mutating,Icon::Return);
            screen.button(958,680,258,"Browse plugins",true,!mutating,Icon::Right);
            if(failed||completed||offerPending||mutating)screen.text(64,773,status,failed?Theme::amber:Theme::muted,21,1152);
        }else{
        screen.text(28,43,"OPEN MPC PLUGIN CATALOG",Theme::ink,31);
        screen.box(setupX,8,setupWidth,40,Theme::panel);screen.icon(setupX+12,16,Icon::Cog,mutating?Theme::muted:Theme::ink);screen.text(setupX+48,36,"SETUP",mutating?Theme::muted:Theme::ink,23,setupWidth-60);
        screen.rect(28,55,1224,1,Theme::line);
        screen.button(28,65,175,"Catalog",view=="catalog",!mutating,Icon::Grid);screen.button(215,65,175,"Local",view=="local",!mutating,Icon::Folder);screen.button(402,65,175,"On MPC",view=="installed",!mutating,Icon::Check);screen.button(589,65,175,"History",view=="history",!mutating,Icon::History);screen.button(1007,65,245,"Return to MPC",false,!mutating,Icon::Return);screen.themeButton(themeX,65,themeWidth,dark,!mutating);
        screen.text(28,159,view=="catalog"?"Browse community plugins":view=="local"?(sourceOnSd?"Install your own plugins from this SD card":"Install your own plugins from this USB"):view=="history"?(sourceOnSd?"Installation history on this SD card - compare with this MPC":"Installation history on this USB - compare with this MPC"):"Manage your installed plugins",Theme::muted,25,800);
        screen.text(850,159,std::to_string(items.size())+" plugins  |  MPC OS 3",Theme::muted,23,400);
        if(confirming){
            screen.box(28,185,1224,390,Theme::panel);
            screen.text(52,239,operation+" "+items[selected].value("name","Plugin")+"?",Theme::ink,34,1150);
            screen.text(52,288,"Your other plugins will stay installed.",Theme::muted,25);
            screen.text(52,333,operation=="Uninstall"?"Remove this plugin from its installation location?":"Verify and install into the plugin installation location?",Theme::ink,25);
            screen.text(52,371,manager.synths.empty()?"No plugin installation location selected. Open SETUP.":manager.synths.string(),Theme::muted,22,1150);
            screen.button(28,395,300,"Confirm "+operation,true,!busy);screen.button(345,395,220,"Cancel");
        } else {
            for(int row=0;row<6;++row){int index=page*6+row;if(index>=int(items.size()))break;const auto& item=items[index];int x=28+(row%2)*620,y=185+(row/2)*130;
                const bool blocked=!item.value("blocked_reason","").empty();
                screen.box(x,y,604,120,Theme::panel,index==selected?Theme::red:Theme::line);
                if(index==selected)screen.rect(x,y,4,120,Theme::red);
                screen.text(x+18,y+35,item.value("name","Plugin"),Theme::ink,31,420);
                screen.rect(x+455,y+12,132,30,Theme::lcd);screen.text(x+466,y+35,view=="installed"?item.value("installed_version",""):view=="history"?item.value("saved_version",""):item.value("version",""),Theme::white,21,112);
                screen.text(x+18,y+63,"by "+item.value("author",item.value("manufacturer","Not listed")),Theme::muted,21,560);
                screen.badge(x+18,y+77,150,item.value("kind","")=="effect"?"EFFECT":"INSTRUMENT");
                screen.badge(x+178,y+77,140,blocked?"UNAVAILABLE":"MPC OS 3",blocked);
                if(item.value("installed",false)){if(view=="history")screen.text(x+328,y+100,"Installed v"+item.value("installed_version",""),Theme::muted,20,210);screen.icon(x+563,y+84,Icon::Disk,Theme::ink);}
                else if(view=="history")screen.badge(x+328,y+77,250,"NOT INSTALLED");
            }
            if(!initial&&items.empty())screen.wrapped(28,230,!listReady?(failed?status:"Loading plugins..."):view=="catalog"&&!catalogError.empty()?catalogError:"No plugins in this view.",1100,3);
            screen.button(28,605,180,"Previous",false,listReady&&(!busy||work==Work::Preferences)&&page>0,Icon::Left);screen.button(220,605,180,"Next",false,listReady&&(!busy||work==Work::Preferences)&&(page+1)*6<int(items.size()),Icon::Right);screen.text(430,643,"Page "+std::to_string(page+1)+" / "+std::to_string(std::max(1,(int(items.size())+5)/6)),Theme::muted);
            if(selected>=0&&listReady&&(!busy||work==Work::Preferences)) {
                const auto& choice=items[selected];const bool installed=choice.value("installed",false),allowed=choice.value("blocked_reason","").empty();
                if(installed){
                    const auto device=choice.value("device_item",Json::object());const bool removable=device.value("device_target","")==manager.synths.string();screen.button(660,605,250,removable?"Uninstall":"External",false,removable,removable?Icon::Trash:Icon::Folder);
                    screen.button(922,605,330,choice.value("update_available",false)?"Update":"Installed",true,allowed&&choice.value("update_available",false),Icon::Download);
                }else screen.button(830,605,422,allowed?"Download & install":"Unavailable",true,allowed,Icon::Download);
            }
        }
        screen.box(28,682,1224,98,Theme::lcd,Theme::lcd);
        screen.wrapped(46,713,confirming?"Confirm "+operation+" to continue, or Cancel.":status,1180,2,failed&&!confirming?0xffffbd91:Theme::white,23);
        if(completed&&!busy&&!confirming)screen.text(46,761,"Choose another plugin to download and install, or Return to MPC.",Theme::white,22,1180);

        }
        if((!welcome&&(busy||progressFading))||(welcome&&(mutating||progressFading))){float value;{std::lock_guard<std::mutex> lock(progressMutex);value=progressValue;}const bool working=welcome?mutating:busy;const float t=working?1.0f:1.0f-float(fadeElapsed)/600;const float opacity=t*t*(3-2*t);const auto background=welcome?Theme::background:Theme::lcd;const int x=welcome?64:46,y=welcome?784:770,width=welcome?1152:1180;auto faded=[&](uint32_t color){uint32_t result=0xff000000;for(int shift:{0,8,16})result|=uint32_t(((color>>shift)&255)*opacity+((background>>shift)&255)*(1-opacity))<<shift;return result;};screen.rect(x,y,width,8,faded(Theme::line));if(!working&&completed)screen.rect(x,y,width,8,faded(Theme::red));else if(value>=0)screen.rect(x,y,int(width*std::clamp(value,0.0f,1.0f)),8,faded(Theme::red));else screen.rect(x+int(((working?SDL_GetTicks():progressEnded)/4)%(width-140)),y,140,8,faded(Theme::red));}
        if(flow&&!welcome&&!progressFading){const auto pixel=screen.pixels[777*1280+46];if(busy?(pixel!=Theme::line&&pixel!=Theme::red):pixel!=Theme::lcd)throw std::runtime_error("Progress bar height or cleanup failed");}
        screen.present();++presents;dirty=false;
        }
        if(flow&&welcome&&!busy&&!returning){
            if(welcomeStep==0){click(1070,565);++welcomeStep;}
            else if(welcomeStep==1){if(offerPending||mutating||fs::exists(manager.session/"download.zip"))throw std::runtime_error("Unavailable welcome installation bypassed its release check");click(170,650);++welcomeStep;}
            else if(welcomeStep==2){if(!hideNext||!manager.hideWelcome())throw std::runtime_error("Welcome checkbox failed");click(1100,705);++welcomeStep;}
        }else if(flow&&!busy&&!progressFading&&!returning) {
            switch(step++) {
                case 0: if(welcomeStep&&(!manager.hideWelcome()||welcome))throw std::runtime_error("Welcome dismissal failed");click(320,90);break;
                case 1: click(100,215);break;
                case 2: click(950,630);break;
                case 3: click(400,420);break;
                case 4: if(confirming)throw std::runtime_error("Cancel failed");click(950,630);break;
                case 5: click(100,420);break;
                case 6: if(items.size()!=1||!items[0].value("installed",false))throw std::runtime_error("UI installation failed");click(450,90);break;
                case 7: click(100,215);break;
                case 8: click(750,630);break;
                case 9: click(100,420);break;
                case 10: if(!items.empty())throw std::runtime_error("UI uninstall failed");click(650,90);break;
                case 11: if(view!="history"||items.size()!=1)throw std::runtime_error("Saved view failed");click(100,215);break;
                case 12: click(950,630);break;
                case 13: click(100,420);break;
                case 14: if(items.size()!=1||!items[0].value("installed",false)||!completed||view!="history")throw std::runtime_error("Reinstall or continued browsing failed");click(450,90);break;
                case 15: click(100,215);break;
                case 16: click(750,630);break;
                case 17: click(100,420);break;
                case 18: if(!items.empty())throw std::runtime_error("Cleanup uninstall failed");click(100,90);break;
                case 19: {const auto sample=manager.catalog.at(0);manager.catalog.clear();for(int i=0;i<13;++i){auto item=sample;item["id"]="pagination-"+std::to_string(i);item["name"]="Pagination fixture "+std::to_string(i);manager.catalog.push_back(item);}reload();break;}
                case 20: click(300,630);break;
                case 21: if(page!=1)throw std::runtime_error("Next page failed");click(300,630);break;
                case 22: if(page!=2)throw std::runtime_error("Third page failed");click(320,90);break;
                case 23: if(view!="local"||page!=0)throw std::runtime_error("Independent tab page failed");click(100,90);break;
                case 24: if(view!="catalog"||page!=2)throw std::runtime_error("Tab page restoration failed");click(930,90);break;
                case 25: if(page!=2||dark==startingDark||manager.darkTheme()!=dark)throw std::runtime_error("Theme changed page or failed persistence");click(930,90);break;
                case 26: if(page!=2||dark!=startingDark||manager.darkTheme()!=dark)throw std::runtime_error("Theme restoration changed page");manager.catalog.resize(7);pages[view]=page;reload();break;
                case 27: if(page!=1)throw std::runtime_error("Shrinking list page clamp failed");click(450,90);break;
                case 28: if(view!="installed"||page!=0)throw std::runtime_error("Empty tab page failed");click(100,90);break;
                case 29: if(view!="catalog"||page!=1)throw std::runtime_error("Clamped page restoration failed");std::fprintf(stderr,"PASS: welcome checkbox persistence and dismissal, touchscreen flow, per-tab page restoration, independent tab pages, theme page preservation and shrinking-list clamp\n");click(1100,90);break;
                default: throw std::runtime_error("Return to MPC failed");
            }
        }
        if(pluginLocationTest&&!busy&&!returning){
            const bool blocked=std::string(testMode)=="plugin-location-blocked",custom=std::string(testMode)=="plugin-location-custom";
            if(step==0&&listReady){click(1070,475);++step;}
            else if(step==1&&sdView){click(540,90);++step;}
            else if(step==2&&pluginLocationView&&!pluginLocationPending){if(!pluginLocationState.value("available",false)||pluginLocationState.value("targets",Json::array()).empty())throw std::runtime_error("Plugin location choices missing");if(blocked&&(!manager.synths.empty()||!pluginLocationState.contains("native_error")))throw std::runtime_error("Blocked native drive silently fell back");capture("plugin-location-automatic.bmp");click(180,420);if(custom){click(550,630);step=6;}else{click(1090,630);++step;}}
            else if(step==3){if(pluginLocationState.value("automatic",true)||manager.synths.empty()||(!custom&&fs::exists(manager.synths))||(custom&&manager.synths.filename()!="My plugins"))throw std::runtime_error("Manual selection changed storage or did not select");capture("plugin-location-manual.bmp");if(blocked){click(120,90);step=5;}else{click(1090,235);++step;}}
            else if(step==4){if(!pluginLocationState.value("automatic",false)||manager.synths.filename()!="Synths")throw std::runtime_error("Automatic button failed");click(120,90);++step;}
            else if(step==6&&pluginLocationState.contains("browser")){capture("plugin-location-folders.bmp");click(180,420);step=7;}
            else if(step==7&&pluginLocationState.contains("browser")){if(fs::path(pluginLocationState.at("browser").at("path").get<std::string>()).filename()!="My plugins")throw std::runtime_error("Folder browser did not enter the selected folder");capture("plugin-location-custom.bmp");click(1090,630);step=3;}
            else if(step==5&&!pluginLocationView){capture("plugin-manager-setup.bmp");click(1100,90);step=100;std::fprintf(stderr,"PASS: plugin installation location labels, automatic/manual selection and no selection-time writes\n");}
        }
#ifdef MPC_MANAGER_SD_FIXTURE
        if(sdTest&&(std::string(testMode)=="sd-updates"||std::string(testMode)=="sd-update-error")&&busy&&work==Work::ManagerCheck){
            if(step==2){probeStarted=SDL_GetTicks();click(120,90);++step;}
            else if(step==3){if(sdView||!welcome||SDL_GetTicks()-probeStarted>250)throw std::runtime_error("Update lookup blocked Back navigation");click(1070,475);++step;}
            else if(step==4){if(!sdView||sdConfirm||mutating||SDL_GetTicks()-probeStarted>250)throw std::runtime_error("Update lookup blocked SETUP reopening");click(1070,430);click(1070,543);++step;}
        }
        if(sdTest&&!busy&&!returning){
            const std::string mode=testMode;
            auto checkTheme=[&](bool compact){const int left=themeX,right=995;const int x=compact?1096:left+16,y=compact?55:82;const auto sample=screen.pixels[(y+12)*1280+x+7];if(sample!=(dark?Theme::panel:Theme::ink))throw std::runtime_error("Theme target glyph is not sun for Light or moon for Dark");if(manager.darkTheme()!=dark)throw std::runtime_error("Theme preference did not persist");if(!compact&&(themeWidth>=181||screen.pixels[65*1280+left-1]!=Theme::background||screen.pixels[65*1280+left]!=Theme::line||screen.pixels[65*1280+right-1]!=Theme::line))throw std::runtime_error("Theme button bounds are not fitted to the labels");};
            if(mode=="sd-unavailable"){
                if(step==0&&listReady){if(sdAvailable)throw std::runtime_error("SD fixture capability unexpectedly available");capture("welcome.bmp");click(1070,475);++step;}
                else if(step==1){if(!sdView||sdFromWelcome!=true||!sdReady)throw std::runtime_error("SETUP unavailable without the SD helper");capture("setup-unavailable.bmp");click(1070,235);click(1070,430);++step;}
                else if(step==2){if(sdPicker||sdConfirm||!hideNext||!manager.hideWelcome())throw std::runtime_error("Unsupported SD install or inverse welcome preference failed");click(1070,543);++step;}
                else if(step==3){if(!managerCheckFailed||managerUpdateStatus.rfind("Check failed: ",0)!=0||!managerUpdate.empty())throw std::runtime_error("Manager check unavailable without SD support");click(120,90);++step;}
                else if(step==4&&welcome){click(1100,705);++step;}
                else if(step==5&&!welcome){click(1150,28);++step;}
                else if(step==6){if(!sdView||sdFromWelcome||!managerCheckFailed||managerUpdateStatus.rfind("Check failed: ",0)!=0)throw std::runtime_error("Catalog SETUP lost update result");click(120,90);++step;}
                else if(step==7&&!sdView){if(welcome)throw std::runtime_error("SETUP Back changed catalog origin");std::fprintf(stderr,"PASS: shared SETUP without SD support, disabled SD install, inverse preference and persistent failed manager check\n");click(1100,90);step=100;}
            }else if(mode=="sd-hidden"){
                if(step==0&&listReady){if(welcome||!hideNext||!manager.hideWelcome())throw std::runtime_error("Saved hidden welcome preference was not loaded");click(1150,28);++step;}
                else if(step==1&&sdView&&sdReady){if(screen.pixels[418*1280+922]!=Theme::background)throw std::runtime_error("Show on opening checkbox ignored saved hidden preference");capture("setup-hidden.bmp");click(1070,430);++step;}
                else if(step==2){if(hideNext||manager.hideWelcome()||screen.pixels[418*1280+922]!=Theme::red)throw std::runtime_error("Show on opening did not save the inverse preference");click(120,90);++step;}
                else if(step==3){if(welcome||sdView)throw std::runtime_error("Preference change unexpectedly switched the current screen");click(1150,28);++step;}
                else if(step==4&&sdReady){if(hideNext)throw std::runtime_error("Reopening SETUP lost the welcome preference");std::fprintf(stderr,"PASS: saved hidden state and Show on opening inverse persistence across SETUP reopen\n");click(1100,90);step=100;}
            }else if(mode=="sd-themes"){
                if(step==0&&listReady){if(!welcome||dark)throw std::runtime_error("Theme fixture did not start on light welcome");checkTheme(true);capture("welcome-light.bmp");click(1150,65);++step;}
                else if(step==1){if(!dark)throw std::runtime_error("Welcome theme toggle failed");checkTheme(true);capture("welcome-dark.bmp");click(1070,475);++step;}
                else if(step==2&&sdReady){checkTheme(false);capture("setup-dark.bmp");click(930,90);++step;}
                else if(step==3){if(dark)throw std::runtime_error("SETUP theme toggle failed");checkTheme(false);capture("setup-light.bmp");click(120,90);++step;}
                else if(step==4&&welcome){checkTheme(true);click(1100,705);++step;}
                else if(step==5&&!welcome){checkTheme(false);if(setupWidth>=184||screen.pixels[8*1280+setupX-1]!=Theme::background||screen.pixels[8*1280+setupX]!=Theme::line||screen.pixels[8*1280+1251]!=Theme::line)throw std::runtime_error("Catalog SETUP bounds are not fitted to its content");click(setupX-2,28);click(990,90);++step;}
                else if(step==6){if(!dark||sdView)throw std::runtime_error("Catalog theme toggle or SETUP hitbox failed");checkTheme(false);capture("catalog-dark.bmp");click(1150,28);++step;}
                else if(step==7&&sdReady){checkTheme(false);click(120,90);++step;}
                else if(step==8&&!sdView){if(welcome)throw std::runtime_error("Theme navigation lost the catalog origin");checkTheme(false);click(930,90);++step;}
                else if(step==9){if(dark)throw std::runtime_error("Catalog theme restoration failed");checkTheme(false);capture("catalog-light.bmp");std::fprintf(stderr,"PASS: sun/Light and moon/Dark target glyphs, theme persistence on welcome, SETUP and catalog\n");click(1100,90);step=100;}
            }else if(mode=="sd-updates"||mode=="sd-update-error"){
                if(step==0&&listReady){click(1070,475);++step;}
                else if(step==1&&sdReady){if(managerUpdateStatus!="Not checked")throw std::runtime_error("Catalog status leaked into the update result");click(1070,543);click(1070,543);++step;}
                else if(step==5&&sdReady){
                    const bool expectedFailure=mode=="sd-update-error";
                    if(managerCheckFailed!=expectedFailure||managerUpdateStatus=="Not checked"||managerUpdateStatus=="Checking GitHub Releases..."||!hideNext||!manager.hideWelcome()||sdConfirm||mutating)throw std::runtime_error("Manager update result or queued preference did not finish safely");
                    if(!expectedFailure&&(!managerUpdate.value("update_available",false)||testReleaseVersion.empty()||managerUpdate.value("latest_version","")!=testReleaseVersion||managerUpdateStatus.find("v"+testReleaseVersion)==std::string::npos))throw std::runtime_error("Latest manager release was not displayed");
                    if(expectedFailure&&(managerUpdateStatus.rfind("Check failed: ",0)!=0||!managerUpdate.empty()))throw std::runtime_error("Manager update error retained a success result");
                    updateTestResult=managerUpdateStatus;capture("setup-update-result.bmp");click(120,90);++step;
                }else if(step==6&&welcome){click(1070,475);++step;}
                else if(step==7&&sdReady){if(managerUpdateStatus!=updateTestResult)throw std::runtime_error("Reopening SETUP lost the manager check result");idlePresents=presents;probeStarted=SDL_GetTicks();++step;}
                else if(step==8&&SDL_GetTicks()-probeStarted>=600){if(presents!=idlePresents)throw std::runtime_error("Idle SETUP keeps redrawing");std::fprintf(stderr,"PASS: asynchronous manager check %s, navigation during lookup, duplicate taps, queued preferences, persistent result and idle redraw suspension\n",mode=="sd-update-error"?"failure":"success");click(1100,90);step=100;}
            }else if(mode=="sd-self-update"||mode=="sd-self-update-fail"){
                const bool expectedFailure=mode=="sd-self-update-fail";
                if(step==0&&listReady){click(1070,475);++step;}
                else if(step==1&&sdReady){click(1070,543);++step;}
                else if(step==2){if(managerCheckFailed||!managerUpdate.value("update_available",false)||!managerUpdate.value("downloadable",false))throw std::runtime_error("Verified manager update offer unavailable");click(1070,613);++step;}
                else if(step==3){if(!managerUpdateConfirm||fs::exists(manager.session/"manager-downloads.log"))throw std::runtime_error("Manager update downloaded before confirmation");capture("manager-update-confirm.bmp");click(520,480);++step;}
                else if(step==4){if(managerUpdateConfirm||fs::exists(manager.session/"manager-downloads.log"))throw std::runtime_error("Cancelled manager update dispatched a download");click(1070,613);++step;}
                else if(step==5){if(!managerUpdateConfirm)throw std::runtime_error("Manager update confirmation did not reopen");click(170,480);click(170,480);click(120,90);click(1100,90);++step;}
                else if(step==6&&!progressFading){
                    if(!sdView||managerUpdateConfirm||mutating||returning||managerUpdated==expectedFailure||completed==expectedFailure||failed!=expectedFailure||managerCheckFailed!=expectedFailure)throw std::runtime_error("Confirmed manager update finished in an unsafe UI state");
                    if(expectedFailure&&managerUpdateStatus.find("Injected manager update publication failure")==std::string::npos)throw std::runtime_error("Manager apply failure was not shown");
                    if(!expectedFailure&&(managerUpdateStatus.find("Return to MPC")==std::string::npos||managerUpdateStatus.find("next shortcut opening")==std::string::npos))throw std::runtime_error("Manager update activation instructions missing");
                    updateTestResult=managerUpdateStatus;capture("manager-update-result.bmp");if(!expectedFailure){click(1070,543);click(1070,613);}++step;
                }else if(step==7){if(managerCheckPending||managerUpdateConfirm||managerUpdateStatus!=updateTestResult)throw std::runtime_error("Successful manager update allowed a repeated install or check");click(120,90);++step;}
                else if(step==8&&welcome){click(1070,475);++step;}
                else if(step==9&&sdReady){if(managerUpdateStatus!=updateTestResult||managerUpdated==expectedFailure)throw std::runtime_error("SETUP reopen lost manager update outcome");std::fprintf(stderr,"PASS: confirmed manager download/apply %s, cancellation, blocked repeat/navigation taps and retained next-open state\n",expectedFailure?"rollback failure report":"success");click(1100,90);step=100;}
            }else if(mode=="sd-empty"){
                if(step==0&&listReady){if(!sdAvailable)throw std::runtime_error("SD fixture capability absent");click(1070,475);++step;}
                else if(step==1&&sdView&&sdReady){if(!sdCards.empty()||!sdSelected.empty())throw std::runtime_error("Empty SD list selected a destination");click(1070,235);++step;}
                else if(step==2){if(!sdPicker)throw std::runtime_error("SD picker did not open");capture("sd-empty.bmp");click(1100,630);++step;}
                else if(step==3){if(sdConfirm)throw std::runtime_error("SD install enabled without a card");click(120,90);++step;}
                else if(step==4){if(sdPicker||!sdView)throw std::runtime_error("SD picker Back did not restore SETUP");click(120,90);++step;}
                else if(step==5&&welcome){click(1100,705);++step;}
                else if(step==6&&!welcome){click(1150,28);++step;}
                else if(step==7&&sdView&&sdReady){click(120,90);++step;}
                else if(step==8&&!sdView){if(welcome)throw std::runtime_error("SETUP Back changed its originating page");std::fprintf(stderr,"PASS: empty SD list, disabled install and welcome/catalog Back\n");click(1100,90);step=100;}
            }else if(mode=="sd-cancel"||mode=="sd-install"){
                if(step==0&&listReady){if(!sdAvailable)throw std::runtime_error("SD fixture capability absent");click(1070,475);++step;}
                else if(step==1&&sdView&&sdReady){if(sdCards.empty()||!sdSelected.empty()||sdState.value("configured",false))throw std::runtime_error("SD selection was not explicit");click(1070,235);++step;}
                else if(step==2){if(!sdPicker)throw std::runtime_error("SD picker did not open");capture("sd-cards.bmp");click(150,220);++step;}
                else if(step==3){if(sdSelected!=sdCards[0].at("uuid").get<std::string>()||sdConfirm)throw std::runtime_error("SD card selection failed");click(1100,630);++step;}
                else if(step==4){if(!sdConfirm)throw std::runtime_error("SD confirmation absent");capture("sd-confirm.bmp");if(mode=="sd-cancel")click(470,450);else{click(170,450);click(170,450);click(1100,90);click(120,90);}++step;}
                else if(step==5){if(returning||sdConfirm||!sdView)throw std::runtime_error("SD mutation/navigation state failed");if(mode=="sd-cancel"){if(sdState.value("configured",false))throw std::runtime_error("Cancelled SD selection installed manager");click(300,90);++step;}else{float value;{std::lock_guard<std::mutex> lock(progressMutex);value=progressValue;}if(!completed||failed||sdPicker||!sdState.value("installed",false)||sdState.value("uuid","")!=sdSelected||value!=1)throw std::runtime_error("SD installation did not complete safely");capture("sd-installed.bmp");std::fprintf(stderr,"PASS: explicit SD selection, confirmation, installation progress and blocked repeat/navigation taps\n");click(1100,90);step=100;}}
                else if(step==6&&sdReady){if(sdState.value("configured",false)||!sdSelected.empty())throw std::runtime_error("Cancelled SD install changed persistent state");click(120,90);++step;}
                else if(step==7){if(sdPicker||!sdView)throw std::runtime_error("Cancelled SD picker Back did not restore SETUP");click(120,90);++step;}
                else if(step==8&&welcome){std::fprintf(stderr,"PASS: SD selection cancellation and refresh preserve installation state\n");click(170,705);step=100;}
            }else if(mode=="sd-remove"||mode=="sd-remove-current"){
                if(step==0&&listReady){click(1070,475);++step;}
                else if(step==1&&sdView&&sdReady){if(!sdState.value("configured",false)||!sdState.value("can_remove",false))throw std::runtime_error("SD removal fixture unavailable");click(1070,305);++step;}
                else if(step==2){if(!sdConfirm||sdOperation!="Remove")throw std::runtime_error("SD removal confirmation absent");capture("sd-remove-confirm.bmp");click(470,450);++step;}
                else if(step==3){if(sdConfirm||!sdState.value("configured",false))throw std::runtime_error("Cancelled SD removal changed state");click(1070,305);++step;}
                else if(step==4){click(170,450);click(170,450);click(1100,90);step=mode=="sd-remove-current"?99:5;}
                else if(step==99)throw std::runtime_error("Current SD removal failed to return to MPC");
                else if(step==5){if(returning||failed||!completed||sdState.value("configured",false))throw std::runtime_error("SD removal did not complete safely");std::fprintf(stderr,"PASS: SD removal confirmation, cancellation and completion\n");click(1100,90);step=100;}
            }else throw std::runtime_error("Unknown SD UI fixture mode");
        }
#endif
        if(direct){
            const std::string mode=testMode;const bool disabled=mode=="welcome-current"||mode=="welcome-newer"||mode=="welcome-unknown";
            if(step==0&&welcome&&!busy&&listReady){if(disabled&&(welcomeOffer.value("label","")!="Installed"||welcomeOffer.value("allowed",true)))throw std::runtime_error("Installed welcome button was not disabled");if(mode=="welcome-update"&&welcomeOffer.value("label","")!="Update")throw std::runtime_error("Welcome update label was not shown");click(1070,565);step=disabled?4:1;}
            else if(step==1&&busy&&mutating&&work==Work::Offer){click(1070,565);click(1070,565);click(1100,705);click(170,705);++step;}
            else if(step==4&&!busy&&listReady){if(!welcome||returning||offerPending||mutating||failed||completed)throw std::runtime_error("Disabled welcome button dispatched work");std::fprintf(stderr,"PASS: installed welcome button ignores taps\n");click(170,705);step=3;}
            else if(step==2&&!busy&&!progressFading&&listReady){const bool expectedFailure=std::string(testMode)=="welcome-install-fail";if(!welcome||returning||confirming||failed!=expectedFailure||completed==expectedFailure||(!expectedFailure&&(welcomeOffer.value("label","")!="Installed"||welcomeOffer.value("allowed",true))))throw std::runtime_error("Direct welcome installation did not finish safely");std::fprintf(stderr,"PASS: direct welcome installation %s; repeat taps and navigation blocked during work\n",expectedFailure?"refused":"completed");click(170,705);++step;}
        }
        if(responsive){
            const Uint32 now=SDL_GetTicks();
            if(step==0&&!busy&&listReady){click(1070,565);click(170,650);click(1100,705);probeStarted=now;++step;}
            else if(step==1){if(welcome||!hideNext||offerPending||status.find("Checking the catalog-selected")!=std::string::npos||now-probeStarted>250)throw std::runtime_error("Welcome input stalled or optional lookup was not cancelled");click(930,90);++step;}
            else if(step==2&&!busy&&listReady){if(!manager.hideWelcome()||manager.darkTheme()==startingDark)throw std::runtime_error("Queued preferences failed");click(320,90);click(320,90);click(320,90);probeStarted=now;++step;}
            else if(step==3){if(view!="local"||!busy||work!=Work::List)throw std::runtime_error("Local background read did not start");click(650,90);++step;}
            else if(step==4){if(view!="history"||now-probeStarted>250)throw std::runtime_error("Navigation blocked on local ZIP read");click(100,90);click(930,90);++step;}
            else if(step==5){if(view!="catalog"||dark!=startingDark||!busy||activeView!="local"||now-probeStarted>250)throw std::runtime_error("Latest tab or theme input was dropped: view="+view+", active="+activeView+", busy="+std::to_string(busy)+", elapsed="+std::to_string(now-probeStarted));click(950,630);++step;}
            else if(step==6&&!busy&&listReady){if(view!="catalog"||manager.darkTheme()!=startingDark||items.empty()||items[0].at("id")!=manager.catalog[0].at("id"))throw std::runtime_error("Stale background list replaced the selected tab");const auto sample=manager.catalog.at(0);manager.catalog.clear();for(int i=0;i<13;++i){auto item=sample;item["id"]="ui-probe-"+std::to_string(i);item["name"]="UI probe "+std::to_string(i);manager.catalog.push_back(item);}reload();++step;}
            else if(step==7&&!busy&&listReady){click(100,215);click(950,630);click(930,90);probeStarted=now;++step;}
            else if(step==8){if(!confirming||!busy||work!=Work::Preferences)throw std::runtime_error("Delayed preference-save probe did not start");click(100,420);click(400,420);click(100,215);++step;}
            else if(step==9){if(confirming||selected!=0||!busy||work!=Work::Preferences)throw std::runtime_error("Cancel or card tap dropped during preference save");click(300,630);++step;}
            else if(step==10){if(page!=1||selected!=-1||!busy||work!=Work::Preferences)throw std::runtime_error("Next page tap dropped during preference save");click(100,215);++step;}
            else if(step==11){if(selected!=6||!busy||work!=Work::Preferences)throw std::runtime_error("Second-page card tap dropped during preference save");click(100,630);++step;}
            else if(step==12){if(page!=0||selected!=-1||!busy||work!=Work::Preferences)throw std::runtime_error("Previous page tap dropped during preference save");std::fprintf(stderr,"PASS: card, Next, Previous and Cancel accepted during preference save; disabled Confirm retained (%u ms)\n",now-probeStarted);++step;}
            else if(step==13&&!busy){click(930,90);++step;}
            else if(step==14&&!busy){if(manager.darkTheme()!=startingDark)throw std::runtime_error("Theme restoration failed");idlePresents=presents;probeStarted=now;++step;}
            else if(step==15&&now-probeStarted>=600){if(presents!=idlePresents)throw std::runtime_error("Idle manager keeps redrawing");click(320,90);++step;}
            else if(step==16&&busy&&work==Work::List){click(1100,90);++step;}
            else if(step==17){if(!returning)throw std::runtime_error("Return input was dropped during a read");++step;}
        }
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_QUIT)quitting=1;
            if(event.type==SDL_FINGERDOWN||event.type==SDL_FINGERMOTION){lastTouch=SDL_GetTicks();continue;}
            int x,y;
            if(event.type==SDL_FINGERUP){const auto point=screen.geometry.input(event.tfinger.x*screen.geometry.width(),event.tfinger.y*screen.geometry.height());x=point.first;y=point.second;}
            else if(event.type==SDL_MOUSEBUTTONUP&&event.button.which!=SDL_TOUCH_MOUSEID){const auto point=screen.geometry.input(event.button.x,event.button.y);x=point.first;y=point.second;}
            else continue;
            lastTouch=SDL_GetTicks();dirty=true;
            if(returning)continue;
            if(sdView){
                if(mutating)continue;
                if(y>=65&&y<123){if(x>=28&&x<208){closeSd();continue;}if(x>=220&&x<400&&(sdAvailable||pluginLocationView)&&!busy&&!sdConfirm&&!managerUpdateConfirm){if(pluginLocationView){pluginLocationPending=true;pluginLocationChoice.clear();pluginLocationPage=0;status="Checking plugin installation locations...";}else{sdPending=true;sdReady=false;sdSelected.clear();status="Checking SD installation...";}failed=completed=false;continue;}if(x>=412&&x<712&&manager.arch=="aarch64"&&!pluginLocationView&&!busy&&!sdPicker&&!sdConfirm&&!managerUpdateConfirm){pluginLocationView=true;pluginLocationPending=true;pluginLocationChoice.clear();pluginLocationPage=0;status="Checking plugin installation locations...";continue;}if(x>=themeX&&x<995){dark=!dark;Theme::set(dark);themePending=true;continue;}if(x>=1007){returnToMpc();break;}}
                if(pluginLocationView){
                    if(busy)continue;
                    const auto browser=pluginLocationState.value("browser",Json::object());const bool browsing=!browser.empty();
                    const auto targets=browsing?browser.value("folders",Json::array()):pluginLocationState.value("targets",Json::array());
                    bool selectAutomatic=!browsing&&y>=207&&y<265&&x>=918&&x<1228&&pluginLocationState.contains("native");
                    auto browse=[&](const std::string& path){pluginBrowsePath=path;pluginBrowsePending=true;pluginLocationPage=0;pluginLocationChoice.clear();};
                    if(browsing&&y>=207&&y<265&&x>=918&&x<1228&&!browser.value("parent","").empty()){browse(browser.at("parent"));continue;}
                    if(y>=394&&y<586&&x>=28&&x<1252&&(y-394)%64<56){const int index=pluginLocationPage*3+(y-394)/64;if(index<int(targets.size())){if(browsing){browse(targets[index]);continue;}pluginLocationChoice=targets[index].at("path").get<std::string>();}}
                    bool selectManual=false;
                    if(y>=605&&y<663){if(x>=28&&x<208&&pluginLocationPage>0){--pluginLocationPage;pluginLocationChoice.clear();}else if(x>=220&&x<400&&(pluginLocationPage+1)*3<int(targets.size())){++pluginLocationPage;pluginLocationChoice.clear();}else if(!browsing&&x>=412&&x<712&&!pluginLocationChoice.empty()){for(const auto& target:targets)if(target.at("path")==pluginLocationChoice){browse(target.at("drive"));break;}continue;}else if(x>=942&&x<1252&&(browsing?browser.value("selectable",false):!pluginLocationChoice.empty()))selectManual=true;}
                    if(selectAutomatic||selectManual){const std::string target=selectAutomatic?"":browsing?browser.at("path").get<std::string>():pluginLocationChoice;busy=mutating=true;work=Work::PluginLocationSelect;status="Checking plugin installation location...";job=std::async(std::launch::async,[&,target]{manager.selectPluginStorage(target);Result result;result.pluginLocation=manager.pluginStorageInfo();result.offer=currentOffer();return result;});}
                    continue;
                }
                if(managerUpdateConfirm){
                    if(y>=455&&y<513&&x>=410&&x<630){managerUpdateConfirm=false;continue;}
                    if(y>=455&&y<513&&x>=52&&x<392&&!busy&&!managerUpdated){
                        const Json checkedRelease=managerUpdate;busy=mutating=true;work=Work::ManagerUpdate;failed=completed=managerCheckFailed=false;progressFading=false;amount(-1);status="Preparing the manager update...";
                        job=std::async(std::launch::async,[&,checkedRelease]{
                            const auto staged=manager.stageManagerUpdate(checkedRelease,progress,[&](float value){amount(value<0?value:0.55f*value);});Result result;
#ifdef MPC_MANAGER_SD_FIXTURE
                            ManagerUpdateFixture fixture(manager.usb,manager.session,staged);
                            if(testMode&&std::string(testMode)=="sd-self-update-fail")fixture.paths.beforePublish=[](size_t index){if(index==1)throw std::runtime_error("Injected manager update publication failure");};
                            result.managerUpdate=applyManagerUpdate(manager.usb,manager.session,staged,fixture.paths,progress,[&](float value){amount(value<0?value:0.55f+0.45f*value);});
#else
                            result.managerUpdate=applyManagerUpdate(manager.usb,manager.session,staged,progress,[&](float value){amount(value<0?value:0.55f+0.45f*value);});
#endif
                            std::error_code cleanupError;fs::remove_all(fs::path(staged.at("bundle_root").get<std::string>()).parent_path(),cleanupError);return result;
                        });
                    }
                    continue;
                }
                if(sdConfirm){
                    if(y>=425&&y<483&&x>=380&&x<600){sdConfirm=false;continue;}
                    if(y>=425&&y<483&&x>=52&&x<362&&!busy){const bool remove=sdOperation=="Remove";const std::string uuid=sdSelected;sdSourceRemoval=remove&&sdState.value("current",false);busy=mutating=true;work=remove?Work::SdRemove:Work::SdInstall;failed=completed=false;progressFading=false;amount(-1);status=remove?"Removing the SD installation...":"Checking the selected SD card...";job=std::async(std::launch::async,[&,remove,uuid]{if(remove)sdInstaller.remove(progress,amount);else sdInstaller.install(uuid,progress,amount);Result result;result.storage=sdInstaller.inspect();return result;});}
                    continue;
                }
                if(!sdPicker){
                    if(y>=396&&y<464&&x>=918&&x<1228){hideNext=!hideNext;hidePending=true;failed=false;continue;}
                    if(y>=514&&y<572&&x>=918&&x<1228&&!managerUpdated&&!managerCheckPending&&!(busy&&work==Work::ManagerCheck)){managerCheckPending=true;managerCheckFailed=false;managerUpdateStatus="Checking GitHub Releases...";continue;}
                    if(y>=584&&y<642&&x>=918&&x<1228&&!busy&&!managerCheckPending&&!managerUpdated&&managerUpdate.value("update_available",false)&&managerUpdate.value("downloadable",false)){managerUpdateConfirm=true;continue;}
                }
                if(!sdAvailable||!sdReady||busy)continue;
                if(sdPicker){
                    if(y>=185&&y<577&&x>=28&&x<1252&&(y-185)%98<90){const int index=sdPage*4+(y-185)/98;if(index<int(sdCards.size()))sdSelected=sdCards[index].value("uuid","");}
                    if(y>=605&&y<663){if(x>=28&&x<208&&sdPage>0){--sdPage;sdSelected.clear();}else if(x>=220&&x<400&&(sdPage+1)*4<int(sdCards.size())){++sdPage;sdSelected.clear();}else if(x>=942&&x<1252&&!sdSelected.empty()){sdOperation="Install";sdConfirm=true;}}
                }else if(x>=918&&x<1228){
                    if(y>=207&&y<265){if(sdState.value("configured",false)||sdState.value("can_remove",false)){if(sdState.value("can_remove",false)&&!sdState.value("removing",false)){sdSelected=sdState.value("uuid","");sdOperation="Install";sdConfirm=true;}}else{sdPicker=true;sdSelected.clear();}}
                    else if(y>=277&&y<335&&sdState.value("can_remove",false)){sdOperation="Remove";sdConfirm=true;}
                }
                continue;
            }
            if(welcome){
                if(mutating)continue;
                if(y>=48&&y<86&&x>=1084&&x<1216){dark=!dark;Theme::set(dark);themePending=true;}
                else if(y>=628&&y<671&&x>=64&&x<600){hideNext=!hideNext;hidePending=true;failed=false;}
                else if(y>=538&&y<596&&x>=958&&x<1188&&!offerPending&&!(busy&&work==Work::Offer)&&welcomeOffer.value("allowed",false)){offerPending=true;failed=false;status="Checking the catalog-selected " MPC_PM_NAME " release...";}
                else if(y>=446&&y<504&&x>=958&&x<1188){openSd();continue;}
                else if(y>=680&&y<738){if(x>=64&&x<334){returnToMpc();break;}if(x>=958&&x<1216){welcome=false;offerPending=false;failed=false;status=listReady?(catalogError.empty()?"Catalog ready. Choose a package.":catalogError):"Loading plugins...";}}
                continue;
            }
            if(mutating)continue;
            if(x>=setupX&&x<1252&&y>=8&&y<48){openSd();continue;}
            if(y>=65&&y<123){if(x>=themeX&&x<995){dark=!dark;Theme::set(dark);themePending=true;continue;}if(x>=1007){returnToMpc();break;}if(x>=28&&x<764){std::string target;if(x<203)target="catalog";else if(x>=215&&x<390)target="local";else if(x>=402&&x<577)target="installed";else if(x>=589)target="history";else continue;if(target==view&&!failed)continue;pages[view]=page;view=target;reload();completed=false;failed=false;status="Loading plugins...";}continue;}
            if(!listReady||(busy&&work!=Work::Preferences))continue;
            if(confirming) {
                if(y>=395&&y<453&&x>=345&&x<565){confirming=false;continue;}
                if(!busy&&y>=395&&y<453&&x>=28&&x<328){Json chosen=items[selected];const bool remove=operation=="Uninstall";busy=mutating=true;work=Work::Mutation;confirming=false;completed=false;failed=false;progressFading=false;amount(-1);status=remove?(sourceOnSd?"Uninstalling. Keep the SD card inserted...":"Uninstalling. Keep the USB inserted..."):"Preparing package checks...";job=std::async(std::launch::async,[&,chosen,remove]{if(remove)manager.remove(chosen);else manager.install(chosen,progress,amount);return Result{};});}
                continue;
            }
            if(y>=185&&y<575&&x>=28&&x<1252){int row=(y-185)/130,col=(x-28)/620;if((x-28)%620>=604||(y-185)%130>=120)continue;int index=page*6+row*2+col;if(index<int(items.size())){selected=index;if(!items[index].value("blocked_reason","").empty())status="Download blocked: "+items[index].value("blocked_reason","");else if(!items[index].value("summary","").empty()){status=items[index].value("summary","");completed=false;}}}
            if(y>=605&&y<663){if(x>=28&&x<208&&page>0){--page;selected=-1;}else if(x>=220&&x<400&&(page+1)*6<int(items.size())){++page;selected=-1;}else if(selected>=0){const auto& choice=items[selected];const bool installed=choice.value("installed",false);
                if(installed&&x>=660&&x<910){if(choice.at("device_item").value("device_target","")!=manager.synths.string()){status="Use this plugin's original installer to remove it.";continue;}operation="Uninstall";confirming=true;}
                else if(!choice.value("blocked_reason","").empty())continue;
                else if(installed&&x>=922&&choice.value("update_available",false)){operation="Update";confirming=true;}
                else if(!installed&&x>=830){operation="Install";confirming=true;}
            }}
        }
        if(test&&((!flow&&!responsive&&!direct&&!sdTest&&!pluginLocationTest&&!busy&&++frames==30)||((flow||direct||sdTest||pluginLocationTest)&&quitting))){capture("screen.bmp");quitting=1;}
        startNext();
        SDL_Delay(30);
    }
    if(busy)job.get();
#ifdef MPC_MANAGER_SD_FIXTURE
    if(sdTest&&std::string(testMode)=="sd-remove-current"){if(step!=99||failed||!returning||!sdSourceRemoval||status!="Returning to MPC...")throw std::runtime_error("Current SD removal was mistaken for USB removal");std::fprintf(stderr,"PASS: current SD source removal returns to MPC without a USB removal error\n");step=100;}
#endif
    if(pluginLocationTest&&step!=100)throw std::runtime_error("Plugin location UI test did not finish");
    if(sdTest&&step!=100)throw std::runtime_error("SD UI test did not finish");
    if(direct&&step!=3)throw std::runtime_error("Direct installation test did not finish");
    if(flow&&step!=30)throw std::runtime_error("Interface navigation test did not finish");
    if(responsive){if(step!=18)throw std::runtime_error("Responsiveness test did not finish");std::fprintf(stderr,"PASS: welcome preferences, delayed ZIP navigation, latest-tab precedence, theme input, idle redraw suspension and safe Return during read\n");}
    return 0;
}
int main(int argc,char**argv){try{return app(argc,argv);}catch(const std::exception&e){std::fprintf(stderr,"Manager error: %s\n",e.what());return 1;}}
