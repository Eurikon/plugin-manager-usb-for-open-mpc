#include "backend.hpp"
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

namespace Theme {
uint32_t background=0xffe9e7e2,panel=0xfff7f6f3,line=0xffcdcac2,ink=0xff1b1c1f,muted=0xff64666d,red=0xffd63a30,amber=0xff7a5600,warning=0xfff5ead1;
constexpr uint32_t white=0xfff4f4f4,lcd=0xff1b1c1f;
void set(bool dark){background=dark?0xff242321:0xffe9e7e2;panel=dark?0xff302e2b:0xfff7f6f3;line=dark?0xff55514b:0xffcdcac2;ink=dark?0xfff4f0e8:0xff1b1c1f;muted=dark?0xffbcb6ad:0xff64666d;red=dark?0xffd94a3f:0xffd63a30;amber=dark?0xffffcc70:0xff7a5600;warning=dark?0xff493c28:0xfff5ead1;}
}
static std::atomic<sig_atomic_t> quitting{0};
static_assert(std::atomic<sig_atomic_t>::is_always_lock_free);
static void stop(int) {quitting=1;}
enum class Icon { None, Grid, Folder, Check, History, Return, Download, Trash, Left, Right, Moon, Disk };
struct Screen {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* texture=nullptr;
    FT_Library library=nullptr;FT_Face face=nullptr;std::vector<uint32_t> pixels;
    Screen(const std::string& font):pixels(1280*800,0xff181c22) {
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"0");
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0) throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("MPC PLUGIN INSTALLER",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,800,1280,SDL_WINDOW_FULLSCREEN_DESKTOP);
        if(!window)throw std::runtime_error(SDL_GetError());
        renderer=SDL_CreateRenderer(window,-1,0);if(!renderer)throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer,800,1280);
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
        else if(kind==Icon::Disk){rect(x,y,24,2,color);rect(x,y,2,24,color);rect(x+22,y,2,24,color);rect(x,y+22,24,2,color);rect(x+6,y+2,12,7,color);rect(x+5,y+13,14,2,color);rect(x+5,y+13,2,9,color);rect(x+17,y+13,2,9,color);}
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

    void badge(int x,int y,int w,const std::string& title,bool warning=false){box(x,y,w,30,warning?Theme::warning:Theme::panel,warning?0xffe0a100:Theme::line);text(x+9,y+22,title,warning?Theme::amber:Theme::muted,19,w-14);}
    void wrapped(int x,int y,const std::string& value,int width,int lines,uint32_t color=Theme::ink,int size=23) {
        FT_Set_Pixel_Sizes(face,0,size);std::istringstream words(value);std::string word,line;int used=0,row=0;
        while(words>>word){int wordWidth=0;for(unsigned char c:word){if(!FT_Load_Char(face,c,FT_LOAD_DEFAULT))wordWidth+=face->glyph->advance.x>>6;}
            if(!line.empty()&&used+wordWidth+size/3>width){text(x,y+row*(size+7),line,color,size,width);if(++row>=lines)return;line.clear();used=0;}
            if(!line.empty()){line+=' ';used+=size/3;}line+=word;used+=wordWidth;
        }
        if(row<lines)text(x,y+row*(size+7),line,color,size,width);
    }
    void present(){SDL_UpdateTexture(texture,nullptr,pixels.data(),1280*4);SDL_RenderClear(renderer);SDL_Rect destination{-240,240,1280,800};SDL_RenderCopyEx(renderer,texture,nullptr,&destination,270,nullptr,SDL_FLIP_NONE);SDL_RenderPresent(renderer);}
};
static int app(int argc,char**argv) {
    if(argc!=6)throw std::runtime_error("Usage: manager USB SESSION SYNTHS SETTINGS FONT");
    Manager manager(argv[1],argv[2],argv[3],argv[4]);bool dark=false;try{dark=manager.darkTheme();}catch(const std::exception& e){std::fprintf(stderr,"Theme preference: %s\n",e.what());}Theme::set(dark);Screen screen(argv[5]);
    bool welcome=true,hideNext=false;try{welcome=!manager.hideWelcome();}catch(const std::exception& e){std::fprintf(stderr,"Welcome preference: %s\n",e.what());}
    signal(SIGTERM,stop);signal(SIGINT,stop);
    std::mutex progressMutex;std::string progressText;float progressValue=-1;
    auto amount=[&](float value){std::lock_guard<std::mutex> lock(progressMutex);progressValue=value;};
    auto progress=[&](const std::string& text){std::lock_guard<std::mutex> lock(progressMutex);progressText=text;};
    struct Result {std::vector<Json> items;std::string status,catalogError;Json offer=Json::object();};
    enum class Work {Open,List,Offer,Preferences,Mutation};
    std::string view="catalog",status="Loading catalog once for this session...",catalogError;std::vector<Json> items;
    std::map<std::string,int> pages;int selected=-1,page=0;bool confirming=false,busy=true,initial=true,completed=false,failed=false,mutating=false,listReady=false,listPending=false,returning=false,dirty=true,finishFade=false;std::string operation;
    bool offerPending=false,themePending=false,hidePending=false,savedDark=dark,savedHidden=!welcome,activeDark=dark,activeHidden=!welcome;
    bool saveTheme=false,saveHidden=false;std::string activeView;unsigned long request=0,activeRequest=0;
    Json welcomeOffer={{"label","Install"},{"allowed",false},{"reason","Loading catalog..."}};
    auto currentOffer=[&]() -> Json {for(const auto& item:manager.catalog)if(item.value("id","")=="plugin-manager")return manager.installationOffer(item);return {{"label","Unavailable"},{"allowed",false},{"reason","Plugin Manager is unavailable in the current catalog."}};};
    Work work=Work::Open;
    std::future<Result> job=std::async(std::launch::async,[&]{manager.openCatalog();manager.recordInstalled();return Result{{},manager.catalogError.empty()?"Catalog ready. Choose a package.":manager.catalogError,manager.catalogError,currentOffer()};});
    const char* renderView=std::getenv("MPC_MANAGER_RENDER_VIEW");if(renderView)view=renderView;
    auto reload=[&]{++request;listPending=true;listReady=false;items.clear();selected=-1;page=pages[view];confirming=false;dirty=true;};
    Uint32 lastTouch=SDL_GetTicks(),progressEnded=0;bool progressFading=false;
    auto startNext=[&]{
        if(busy||quitting)return;
        if(themePending||hidePending){
            work=Work::Preferences;busy=true;saveTheme=themePending;saveHidden=!saveTheme&&hidePending;activeDark=dark;activeHidden=hideNext;if(saveTheme)themePending=false;if(saveHidden)hidePending=false;
            job=std::async(std::launch::async,[&,theme=saveTheme,hidden=saveHidden,darkValue=activeDark,hideValue=activeHidden]{if(theme)manager.setDarkTheme(darkValue);if(hidden)manager.setHideWelcome(hideValue);return Result{};});
        }else if(offerPending&&!returning){
            work=Work::Offer;busy=mutating=true;offerPending=false;completed=false;failed=false;progressFading=false;amount(-1);
            job=std::async(std::launch::async,[&]{manager.installLatestPlugin("plugin-manager","poloq-instruments/mpc-vst-manager",progress,amount);return Result{};});
        }else if(listPending&&!returning){
            work=Work::List;busy=true;activeView=view;activeRequest=request;listPending=false;
            job=std::async(std::launch::async,[&,requestedView=activeView]{return Result{manager.list(requestedView),{}, {},currentOffer()};});
        }
    };
    auto returnToMpc=[&]{returning=true;offerPending=false;confirming=false;status="Returning to MPC...";dirty=true;};
    const char* testMode=std::getenv("MPC_MANAGER_UI_TEST");
    const bool test=testMode!=nullptr, flow=test&&(std::string(testMode)=="flow"||std::string(testMode)=="touch-flow"),responsive=test&&std::string(testMode)=="responsive",direct=test&&std::string(testMode).rfind("welcome-",0)==0;int frames=0,step=0,welcomeStep=0,presents=0,idlePresents=0;Uint32 probeStarted=0;const bool startingDark=dark;
    auto click=[&](int x,int y){SDL_Event event{};if(std::string(testMode)=="touch-flow"){event.type=SDL_FINGERUP;int wx,wy,ww,wh;SDL_RenderLogicalToWindow(screen.renderer,float(y),float(1280-x),&wx,&wy);SDL_GetWindowSize(screen.window,&ww,&wh);event.tfinger.x=float(wx)/ww;event.tfinger.y=float(wy)/wh;SDL_PushEvent(&event);return;}event.type=SDL_MOUSEBUTTONUP;event.button.button=SDL_BUTTON_LEFT;event.button.x=y;event.button.y=1280-x;SDL_PushEvent(&event);};
    while(!quitting) {
        if(!fs::is_regular_file(manager.usb/".mpc-manager-id")) {status="USB removed. Returning to MPC.";quitting=1;}
        if(busy&&mutating){std::lock_guard<std::mutex> lock(progressMutex);if(!progressText.empty()){status=progressText;progressText.clear();dirty=true;}}
        if(busy&&job.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
            bool success=true;Result result;
            try{result=job.get();}catch(const std::exception&e){success=false;if((work!=Work::List||(activeRequest==request&&activeView==view))&&(work!=Work::Offer||(welcome&&!returning))){status=std::string("Failed: ")+e.what();failed=true;completed=false;}}
            if(success&&!result.offer.empty())welcomeOffer=std::move(result.offer);
            busy=false;dirty=true;lastTouch=SDL_GetTicks();
            if(work==Work::Open){initial=false;if(success){status=result.status;catalogError=result.catalogError;failed=false;}reload();}
            else if(work==Work::Preferences){
                if(success){if(saveTheme)savedDark=activeDark;if(saveHidden)savedHidden=activeHidden;}
                else {if(saveTheme&&!themePending){dark=savedDark;Theme::set(dark);}if(saveHidden&&!hidePending)hideNext=savedHidden;}
            }else if(work==Work::Offer){
                mutating=false;if(success){status="poloq Plugin Manager installed. Return to MPC to use it on a plugin track.";completed=true;failed=false;welcomeOffer={{"label","Installed"},{"allowed",false},{"reason","Latest version already installed."}};}
                progressEnded=SDL_GetTicks();progressFading=true;reload();
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
        if(welcome){
            screen.text(64,79,"MPC PLUGIN INSTALLER",Theme::ink,38);
            screen.text(64,119,"Community plugins, together in one place",Theme::muted,25);
            screen.button(1020,55,196,dark?"Light":"Dark",false,!mutating,Icon::Moon);
            screen.box(64,155,1152,410,Theme::panel);
            screen.icon(92,168,Icon::Disk,Theme::red);screen.text(134,192,"Your MPC session",Theme::ink,26);
            screen.wrapped(134,226,"Save your project before opening the installer. The MPC application stops while the installer is open and restarts when you return.",1020,2,Theme::muted,22);
            screen.icon(92,264,Icon::Download,Theme::red);screen.text(134,288,"Catalog and local plugins",Theme::ink,26);
            screen.wrapped(134,322,"The catalog loads once per opening. Local ZIP packages on this USB work offline. Catalog data and downloads are cleared when you leave.",1020,2,Theme::muted,22);
            screen.icon(92,360,Icon::Return,Theme::red);screen.text(134,384,"Come back whenever you need",Theme::ink,26);
            screen.wrapped(134,418,"Keep the USB inserted. From MPC, hold SHIFT + MENU for two seconds to reopen the installer.",1020,2,Theme::muted,22);
            screen.rect(92,459,1096,1,Theme::line);
            screen.icon(92,469,Icon::Grid,Theme::red);screen.text(134,492,"Or install \"poloq Plugin Manager\" instead",Theme::ink,26);
            screen.wrapped(134,524,"Manage catalog plugins from an MPC track, without keeping this USB inserted.",790,2,Theme::muted,22);
            screen.button(958,486,230,welcomeOffer.value("label","Install"),false,!offerPending&&!mutating&&welcomeOffer.value("allowed",false),Icon::Right);
            if(!mutating&&!welcomeOffer.value("allowed",false)&&welcomeOffer.value("label","")=="Installed")screen.text(134,553,welcomeOffer.value("reason",""),Theme::muted,19,790);
            screen.box(64,599,30,30,hideNext?Theme::red:Theme::panel);if(hideNext)screen.icon(67,602,Icon::Check,Theme::white);
            screen.text(110,623,"Hide message",mutating?Theme::muted:Theme::ink,25);
            screen.button(64,680,270,"Return to MPC",false,!mutating,Icon::Return);
            screen.button(910,680,306,"Browse plugins",true,!mutating,Icon::Right);
            if(failed||completed||offerPending||mutating)screen.text(64,773,status,failed?Theme::amber:Theme::muted,21,1152);
        }else{
        screen.text(28,43,"MPC OS PLUGIN CATALOG",Theme::ink,31);
        screen.rect(28,55,1224,1,Theme::line);
        screen.button(28,65,175,"Catalog",view=="catalog",!mutating,Icon::Grid);screen.button(215,65,175,"Local",view=="local",!mutating,Icon::Folder);screen.button(402,65,175,"On MPC",view=="installed",!mutating,Icon::Check);screen.button(589,65,175,"History",view=="history",!mutating,Icon::History);screen.button(1007,65,245,"Return to MPC",false,!mutating,Icon::Return);screen.button(802,65,181,dark?"Light":"Dark",false,!mutating,Icon::Moon);
        screen.text(28,159,view=="catalog"?"Browse community plugins":view=="local"?"Install your own plugins from this USB":view=="history"?"Installation history on this USB - compare with this MPC":"Manage your installed plugins",Theme::muted,25,800);
        screen.text(850,159,std::to_string(items.size())+" plugins  |  MPC OS 3",Theme::muted,23,400);
        if(confirming){
            screen.box(28,185,1224,390,Theme::panel);
            screen.text(52,239,operation+" "+items[selected].value("name","Plugin")+"?",Theme::ink,34,1150);
            screen.text(52,288,"Your other plugins will stay installed.",Theme::muted,25);
            screen.text(52,333,operation=="Uninstall"?"Remove this plugin from the SD card?":"Download, verify and install onto the SD card?",Theme::ink,25);
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
            if(welcomeStep==0){click(1070,515);++welcomeStep;}
            else if(welcomeStep==1){if(offerPending||mutating||fs::exists(manager.session/"download.zip"))throw std::runtime_error("Unavailable welcome installation bypassed its release check");click(170,615);++welcomeStep;}
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
                case 24: if(view!="catalog"||page!=2)throw std::runtime_error("Tab page restoration failed");click(850,90);break;
                case 25: if(page!=2||dark==startingDark||manager.darkTheme()!=dark)throw std::runtime_error("Theme changed page or failed persistence");click(850,90);break;
                case 26: if(page!=2||dark!=startingDark||manager.darkTheme()!=dark)throw std::runtime_error("Theme restoration changed page");manager.catalog.resize(7);pages[view]=page;reload();break;
                case 27: if(page!=1)throw std::runtime_error("Shrinking list page clamp failed");click(450,90);break;
                case 28: if(view!="installed"||page!=0)throw std::runtime_error("Empty tab page failed");click(100,90);break;
                case 29: if(view!="catalog"||page!=1)throw std::runtime_error("Clamped page restoration failed");std::fprintf(stderr,"PASS: welcome checkbox persistence and dismissal, touchscreen flow, per-tab page restoration, independent tab pages, theme page preservation and shrinking-list clamp\n");click(1100,90);break;
                default: throw std::runtime_error("Return to MPC failed");
            }
        }
        if(direct){
            const std::string mode=testMode;const bool disabled=mode=="welcome-current"||mode=="welcome-newer"||mode=="welcome-unknown";
            if(step==0&&welcome&&!busy&&listReady){if(disabled&&(welcomeOffer.value("label","")!="Installed"||welcomeOffer.value("allowed",true)))throw std::runtime_error("Installed welcome button was not disabled");if(mode=="welcome-update"&&welcomeOffer.value("label","")!="Update")throw std::runtime_error("Welcome update label was not shown");click(1070,515);step=disabled?4:1;}
            else if(step==1&&busy&&mutating&&work==Work::Offer){click(1070,515);click(1070,515);click(1100,705);click(170,705);++step;}
            else if(step==4&&!busy&&listReady){if(!welcome||returning||offerPending||mutating||failed||completed)throw std::runtime_error("Disabled welcome button dispatched work");std::fprintf(stderr,"PASS: installed welcome button ignores taps\n");click(170,705);step=3;}
            else if(step==2&&!busy&&!progressFading&&listReady){const bool expectedFailure=std::string(testMode)=="welcome-install-fail";if(!welcome||returning||confirming||failed!=expectedFailure||completed==expectedFailure||(!expectedFailure&&(welcomeOffer.value("label","")!="Installed"||welcomeOffer.value("allowed",true))))throw std::runtime_error("Direct welcome installation did not finish safely");std::fprintf(stderr,"PASS: direct welcome installation %s; repeat taps and navigation blocked during work\n",expectedFailure?"refused":"completed");click(170,705);++step;}
        }
        if(responsive){
            const Uint32 now=SDL_GetTicks();
            if(step==0&&!busy&&listReady){click(1070,515);click(170,615);click(1100,705);probeStarted=now;++step;}
            else if(step==1){if(welcome||!hideNext||offerPending||status.find("Checking poloq")!=std::string::npos||now-probeStarted>250)throw std::runtime_error("Welcome input stalled or optional lookup was not cancelled");click(850,90);++step;}
            else if(step==2&&!busy&&listReady){if(!manager.hideWelcome()||manager.darkTheme()==startingDark)throw std::runtime_error("Queued preferences failed");click(320,90);click(320,90);click(320,90);probeStarted=now;++step;}
            else if(step==3){if(view!="local"||!busy||work!=Work::List)throw std::runtime_error("Local background read did not start");click(650,90);++step;}
            else if(step==4){if(view!="history"||now-probeStarted>250)throw std::runtime_error("Navigation blocked on local ZIP read");click(100,90);click(850,90);++step;}
            else if(step==5){if(view!="catalog"||dark!=startingDark||!busy||activeView!="local"||now-probeStarted>250)throw std::runtime_error("Latest tab or theme input was dropped: view="+view+", active="+activeView+", busy="+std::to_string(busy)+", elapsed="+std::to_string(now-probeStarted));click(950,630);++step;}
            else if(step==6&&!busy&&listReady){if(view!="catalog"||manager.darkTheme()!=startingDark||items.empty()||items[0].at("id")!=manager.catalog[0].at("id"))throw std::runtime_error("Stale background list replaced the selected tab");const auto sample=manager.catalog.at(0);manager.catalog.clear();for(int i=0;i<13;++i){auto item=sample;item["id"]="ui-probe-"+std::to_string(i);item["name"]="UI probe "+std::to_string(i);manager.catalog.push_back(item);}reload();++step;}
            else if(step==7&&!busy&&listReady){click(100,215);click(950,630);click(850,90);probeStarted=now;++step;}
            else if(step==8){if(!confirming||!busy||work!=Work::Preferences)throw std::runtime_error("Delayed preference-save probe did not start");click(100,420);click(400,420);click(100,215);++step;}
            else if(step==9){if(confirming||selected!=0||!busy||work!=Work::Preferences)throw std::runtime_error("Cancel or card tap dropped during preference save");click(300,630);++step;}
            else if(step==10){if(page!=1||selected!=-1||!busy||work!=Work::Preferences)throw std::runtime_error("Next page tap dropped during preference save");click(100,215);++step;}
            else if(step==11){if(selected!=6||!busy||work!=Work::Preferences)throw std::runtime_error("Second-page card tap dropped during preference save");click(100,630);++step;}
            else if(step==12){if(page!=0||selected!=-1||!busy||work!=Work::Preferences)throw std::runtime_error("Previous page tap dropped during preference save");std::fprintf(stderr,"PASS: card, Next, Previous and Cancel accepted during preference save; disabled Confirm retained (%u ms)\n",now-probeStarted);++step;}
            else if(step==13&&!busy){click(850,90);++step;}
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
            if(event.type==SDL_FINGERUP){x=std::clamp(int(1280*(1-event.tfinger.y)),0,1279);y=std::clamp(int(800*event.tfinger.x),0,799);}
            else if(event.type==SDL_MOUSEBUTTONUP&&event.button.which!=SDL_TOUCH_MOUSEID){x=1280-event.button.y;y=event.button.x;}
            else continue;
            lastTouch=SDL_GetTicks();dirty=true;
            if(returning)continue;
            if(welcome){
                if(mutating)continue;
                if(y>=55&&y<113&&x>=1020&&x<1216){dark=!dark;Theme::set(dark);themePending=true;}
                else if(y>=580&&y<650&&x>=64&&x<600){hideNext=!hideNext;hidePending=true;failed=false;}
                else if(y>=486&&y<544&&x>=958&&x<1188&&!offerPending&&!(busy&&work==Work::Offer)&&welcomeOffer.value("allowed",false)){offerPending=true;failed=false;status="Checking poloq's latest stable release...";}
                else if(y>=680&&y<738){if(x>=64&&x<334){returnToMpc();break;}if(x>=910&&x<1216){welcome=false;offerPending=false;failed=false;status=listReady?(catalogError.empty()?"Catalog ready. Choose a package.":catalogError):"Loading plugins...";}}
                continue;
            }
            if(mutating)continue;
            if(y>=65&&y<123){if(x>=802&&x<983){dark=!dark;Theme::set(dark);themePending=true;continue;}if(x>=1007){returnToMpc();break;}if(x>=28&&x<764){std::string target;if(x<203)target="catalog";else if(x>=215&&x<390)target="local";else if(x>=402&&x<577)target="installed";else if(x>=589)target="history";else continue;if(target==view&&!failed)continue;pages[view]=page;view=target;reload();completed=false;failed=false;status="Loading plugins...";}continue;}
            if(!listReady||(busy&&work!=Work::Preferences))continue;
            if(confirming) {
                if(y>=395&&y<453&&x>=345&&x<565){confirming=false;continue;}
                if(!busy&&y>=395&&y<453&&x>=28&&x<328){Json chosen=items[selected];const bool remove=operation=="Uninstall";busy=mutating=true;work=Work::Mutation;confirming=false;completed=false;failed=false;progressFading=false;amount(-1);status=remove?"Uninstalling. Keep the USB inserted...":"Preparing package checks...";job=std::async(std::launch::async,[&,chosen,remove]{if(remove)manager.remove(chosen);else manager.install(chosen,progress,amount);return Result{};});}
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
        if(test&&((!flow&&!responsive&&!direct&&!busy&&++frames==30)||((flow||direct)&&quitting))){SDL_Surface* surface=SDL_CreateRGBSurfaceWithFormatFrom(screen.pixels.data(),1280,800,32,1280*4,SDL_PIXELFORMAT_ARGB8888);SDL_SaveBMP(surface,(manager.session/"screen.bmp").c_str());SDL_FreeSurface(surface);quitting=1;}
        startNext();
        SDL_Delay(30);
    }
    if(busy)job.get();
    if(direct&&step!=3)throw std::runtime_error("Direct installation test did not finish");
    if(responsive){if(step!=18)throw std::runtime_error("Responsiveness test did not finish");std::fprintf(stderr,"PASS: welcome preferences, delayed ZIP navigation, latest-tab precedence, theme input, idle redraw suspension and safe Return during read\n");}
    return 0;
}
int main(int argc,char**argv){try{return app(argc,argv);}catch(const std::exception&e){std::fprintf(stderr,"Manager error: %s\n",e.what());return 1;}}
