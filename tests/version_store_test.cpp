#include "checksum.hpp"
#include "version_store.hpp"
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
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}check(rejected,message);}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write version fixture");}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Missing version fixture");return {std::istreambuf_iterator<char>(input),{}};}
pmu::VersionStore::Hash hash=[](const fs::path& path){return pmu::fileSha256(path);};
struct Fixture {
    fs::path root,input,store;
    explicit Fixture(fs::path path):root(std::move(path)),input(root/"input"),store(root/"versions"){fs::create_directories(input);chmod(root.c_str(),0700);chmod(input.c_str(),0700);}
    pmu::GenerationSpec spec(std::string version,std::map<std::string,std::string> files={{"program","payload"},{"assets/data","asset"}}){pmu::GenerationSpec spec{std::move(version),"universal",{}};for(const auto& item:files){save(input/item.first,item.second);spec.files[item.first]=hash(input/item.first);}return spec;}
    pmu::VersionStore open(pmu::VersionStore::Checkpoint checkpoint={}){return {store,geteuid(),hash,std::move(checkpoint)};}
};
void interrupted(Fixture& f,const std::string& point,const std::function<void(pmu::VersionStore&)>& operation){
    const auto child=fork();check(child>=0,"fork interruption fixture");
    if(!child){auto store=f.open([&](const auto& name){if(name==point)kill(getpid(),SIGKILL);});try{operation(store);}catch(...){_exit(90);}_exit(91);}
    int status=0;check(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,"updater killed at the requested durable boundary");
}
}
int main(){
    std::string pattern="/tmp/pmu-version-store.XXXXXX";std::vector<char> buffer(pattern.begin(),pattern.end());buffer.push_back(0);const auto created=mkdtemp(buffer.data());if(!created)return 1;const fs::path root=created;
    try{
        {
            save(root/"hash-empty","");check(hash(root/"hash-empty")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty SHA-256 vector");
            save(root/"hash-abc","abc");check(hash(root/"hash-abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","short SHA-256 vector");
            save(root/"hash-large",std::string(1000000,'a'));check(hash(root/"hash-large")=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","large SHA-256 vector");
            rejects([&]{hash(root/"missing");},"checksum read failure is explicit");rejects([&]{pmu::fileSha256(root/"hash-abc",std::chrono::milliseconds(0));},"checksum rejects an invalid deadline");
            const auto fifo=root/"hash-fifo";check(mkfifo(fifo.c_str(),0600)==0,"checksum FIFO fixture created");const auto started=std::chrono::steady_clock::now();rejects([&]{pmu::fileSha256(fifo,std::chrono::milliseconds(50));},"blocked checksum process is terminated at its deadline");check(std::chrono::steady_clock::now()-started<std::chrono::seconds(3),"blocked checksum cannot hang startup indefinitely");
            const auto child=fork();check(child>=0,"fork closed-descriptor fixture");if(!child){close(0);close(1);close(2);try{_exit(hash(root/"hash-abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"?0:1);}catch(...){_exit(2);}}int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"checksum works when the host has closed standard descriptors");
        }
        {
            Fixture f(root/"normal");auto store=f.open();check(!store.current()&&!store.boot(),"empty store has no active generation");
            auto first=f.spec("1.0.0");first.entrypoints={{"manager","program"}};const auto a=store.stage(f.input,first);check(a.size()==64,"generation has a content-derived identifier");check(read(f.input/"program")=="payload","staging preserves input");
            check(store.stage(f.input,first)==a,"identical staging reuses the verified generation");rejects([&]{store.activate(a);},"activation requires a seeded working release");
            store.seed(a);check(store.current()->id==a&&!store.current()->pending,"initial working generation is healthy");check(store.current()->spec.entrypoints.at("manager")=="program","entrypoint roles are preserved independently of file names");rejects([&]{store.seed(a);},"initialization cannot overwrite an existing selection");
            const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","new program"},{"new/nested/file","new resource"},{"empty",""}}));store.activate(b);
            check(store.current()->id==b&&store.current()->pending,"activation selects a complete pending generation");check(read(f.store/a/"program")=="payload","activation leaves old payload intact");
            check(!fs::exists(f.store/b/"assets/data")&&fs::exists(f.store/b/"new/nested/file"),"new generation handles changed file layout without editing old files");
            rejects([&]{store.confirm(b);},"health confirmation requires an actual boot attempt");rejects([&]{store.activate(a);},"another activation waits for health or recovery");rejects([&]{store.prune();},"unconfirmed activation cannot retire its fallback");
            check(store.boot()->id==b,"first boot attempts the complete new release");rejects([&]{store.confirm(a);},"stale health acknowledgement cannot confirm another version");store.confirm(b);
            rejects([&]{store.activate(a,a);},"stale update preparation cannot replace a subsequently confirmed version");check(store.current()->id==b,"rejected stale activation preserves the newer selected version");
            check(store.boot()->id==b&&!store.current()->pending,"confirmed release survives later boots");
            const auto c=store.stage(f.input,f.spec("3.0.0",{{"other/program","third release"}}));store.activate(c);store.boot();store.confirm(c);save(f.store/"user-file","keep");store.prune();
            check(!fs::exists(f.store/a)&&fs::exists(f.store/b)&&fs::exists(f.store/c),"retirement keeps current and one previous generation");check(read(f.store/"user-file")=="keep","retirement preserves unrelated files");
        }
        for(const std::string damage:{"no-confirm","payload","receipt","extra","link","active-metadata","missing-selection"}){
            Fixture f(root/("fallback-"+damage));auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));store.activate(b);
            if(damage=="no-confirm")store.boot();
            else if(damage=="payload")save(f.store/b/"program","damaged");
            else if(damage=="receipt")save(f.store/b/"generation.json","{}");
            else if(damage=="extra")save(f.store/b/"unowned","keep");
            else if(damage=="link"){fs::remove(f.store/b/"program");fs::create_symlink(f.store/a/"program",f.store/b/"program");}
            else if(damage=="active-metadata")save(f.store/"active.json","truncated");
            else fs::remove(f.store/"active.json");
            check(store.boot()->id==a,"failed or damaged activation automatically selects last working generation");check(read(f.store/a/"program")=="payload","recovery preserves the fallback payload");
        }
        {
            Fixture f(root/"healthy-damage");auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));store.activate(b);store.boot();store.confirm(b);save(f.store/b/"program","damaged");check(store.boot()->id==a,"damaged confirmed release falls back to the retained previous version");save(f.store/"active.json","{}");check(store.boot()->id==a,"restored health record survives subsequent selection damage");
        }
        for(const std::string path:{"../outside","/tmp/outside","a/../../outside","a//b","generation.json/child","a\\b","a\nb"}){
            Fixture f(root/("bad-path-"+std::to_string(checks)));auto store=f.open();auto spec=f.spec("1.0.0");spec.files={{path,std::string(64,'a')}};rejects([&]{store.stage(f.input,spec);},"unsafe generation path is rejected");
        }
        {
            Fixture f(root/"bad-spec");auto store=f.open();auto spec=f.spec("1.0.0");spec.files["program/child"]=std::string(64,'a');rejects([&]{store.stage(f.input,spec);},"file cannot also be a generation directory");spec=f.spec("1.0.0");spec.files["program"]="invalid";rejects([&]{store.stage(f.input,spec);},"malformed digest is rejected");spec=f.spec("1.0.0");spec.files["program"]=std::string(64,'a');rejects([&]{store.stage(f.input,spec);},"source checksum mismatch is rejected");
            spec=f.spec("1.0.0");spec.entrypoints={{"manager","missing"}};rejects([&]{store.stage(f.input,spec);},"entrypoint must refer to an inventoried file");
            spec=f.spec("1.0.0");fs::remove(f.input/"program");fs::create_symlink(f.input/"assets/data",f.input/"program");rejects([&]{store.stage(f.input,spec);},"source symlink is rejected");fs::remove(f.input/"program");check(mkfifo((f.input/"program").c_str(),0600)==0,"FIFO fixture created");rejects([&]{store.stage(f.input,spec);},"source FIFO is rejected without blocking");
        }
        {
            Fixture f(root/"unsafe-store");fs::create_directory(f.root/"elsewhere");fs::create_directory_symlink(f.root/"elsewhere",f.store);rejects([&]{auto store=f.open();},"linked version-store root is rejected");
        }
        {
            Fixture f(root/"media-replaced");auto spec=f.spec("1.0.0");auto store=f.open([&](const auto& point){if(point=="generation-ready"){fs::rename(f.store,f.root/"retained");fs::create_directory(f.store);}});rejects([&]{store.stage(f.input,spec);},"replaced storage prevents publication");check(fs::is_empty(f.store),"replacement storage receives no payload or activation writes");
        }
        {
            Fixture f(root/"unowned-retirement");auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));store.activate(b);store.boot();store.confirm(b);const auto c=store.stage(f.input,f.spec("3.0.0",{{"program","third"}}));store.activate(c);store.boot();store.confirm(c);save(f.store/a/"foreign","preserve");rejects([&]{store.prune();},"retirement refuses unexpected files before deleting anything");check(read(f.store/a/"program")=="payload"&&read(f.store/a/"foreign")=="preserve","unowned collision preserves the entire old generation");
        }
        const std::vector<std::string> metadataPoints={"selection-synced:healthy.json","selection-published:healthy.json","selection-durable:healthy.json","selection-synced:active.json","selection-published:active.json","selection-durable:active.json"};
        for(const auto& point:metadataPoints){
            Fixture f(root/("activate-kill-"+std::to_string(checks)));auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));interrupted(f,point,[&](auto& child){child.activate(b);});auto restarted=f.open();const auto chosen=restarted.boot();check(chosen&&(chosen->id==a||chosen->id==b),"interrupted activation selects only a complete verified generation");if(chosen->pending)check(restarted.boot()->id==a,"unconfirmed post-interruption candidate falls back on next boot");
        }
        for(const std::string operation:{"boot","confirm","reject"})for(const auto& point:metadataPoints){
            if(operation=="boot"&&point.find("healthy.json")!=std::string::npos)continue;
            Fixture f(root/(operation+"-kill-"+std::to_string(checks)));auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));store.activate(b);if(operation=="confirm")store.boot();
            interrupted(f,point,[&](auto& child){if(operation=="boot")child.boot();else if(operation=="confirm")child.confirm(b);else child.reject(b);});auto restarted=f.open();const auto chosen=restarted.boot();check(chosen&&(chosen->id==a||chosen->id==b),"interrupted boot, health or rejection recovers a complete generation");
        }
        for(const std::string point:{"generation-staging","copy:program","copy-durable:program","generation-ready","generation-durable"}){
            Fixture f(root/("stage-kill-"+std::to_string(checks)));auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto next=f.spec("2.0.0",{{"program","second"}});interrupted(f,point,[&](auto& child){child.stage(f.input,next);});auto restarted=f.open();check(restarted.boot()->id==a,"interrupted staging never changes the working version");const auto b=restarted.stage(f.input,next);restarted.activate(b);check(restarted.boot()->id==b,"retry after interrupted staging can activate the complete update");for(const auto& entry:fs::directory_iterator(f.store))check(entry.path().filename().string().rfind(".stage-",0)!=0,"retry clears only its interrupted staging data");
        }
        for(const std::string point:{"retirement-started","retirement-file-durable"}){
            Fixture f(root/("prune-kill-"+std::to_string(checks)));auto store=f.open();const auto a=store.stage(f.input,f.spec("1.0.0"));store.seed(a);const auto b=store.stage(f.input,f.spec("2.0.0",{{"program","second"}}));store.activate(b);store.boot();store.confirm(b);const auto c=store.stage(f.input,f.spec("3.0.0",{{"program","third"}}));store.activate(c);store.boot();store.confirm(c);
            interrupted(f,point,[&](auto& child){child.prune();});auto restarted=f.open();check(restarted.boot()->id==c,"interrupted retirement leaves the working selection intact");restarted.prune();check(!fs::exists(f.store/a)&&fs::exists(f.store/b)&&fs::exists(f.store/c),"retirement resumes after forced termination");for(const auto& entry:fs::directory_iterator(f.store))check(entry.path().filename().string().rfind(".retired-",0)!=0,"resumed retirement clears its recovery directory");
        }
        fs::remove_all(root);std::cout<<"PASS: "<<checks<<" version-store integrity, migration and SIGKILL recovery checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
