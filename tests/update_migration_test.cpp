#include "update_migration.hpp"
#include "source_runtime.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "signing_fixture.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write migration fixture");}
std::string read(const fs::path& path){std::ifstream input(path);if(!input)throw std::runtime_error("Missing migration fixture");return {std::istreambuf_iterator<char>(input),{}};}
auto hash=[](const auto& path){return pmu::fileSha256(path);};
struct Fixture {
    fs::path root,source,helper;
    pmu::test::SigningFixture key;
    std::string firstRuntime,firstHelper,identity;
    pmu::MigrationPayload runtimePayload,helperPayload;
    explicit Fixture(fs::path path):root(std::move(path)),source(root/"USB"),helper(root/"helper"){
        save(source/".mpc-manager-id","mpc-usb-manager-v1\n");
        save(source/"plugin-history.json","history");save(source/"local-plugins/keep","user package");save(helper/"controller.ini","controller configuration");
        fs::create_directories(source/"autorun/runtime");fs::create_directories(helper);
        auto runtime=payload("initial-runtime","1.0.0","runtime",false);auto h=payload("initial-helper","1.0.0","helper",false);
        auto runtimeStore=store(false);firstRuntime=runtimeStore.stage(runtime.directory,runtime.specification,runtime.authorization);runtimeStore.seed(firstRuntime);
        auto helperStore=store(true);firstHelper=helperStore.stage(h.directory,h.specification,h.authorization);helperStore.seed(firstHelper);
        runtimePayload=payload("next-runtime","4.0.0","runtime",true);helperPayload=payload("next-helper","4.0.0","helper",true);
        pmu::SourceRuntime current(source,"armv7",key.publicKey());identity=current.identity();
    }
    pmu::MigrationPayload payload(const std::string& folder,const std::string& version,const std::string& kind,bool nested){
        const auto path=root/folder;const auto program=nested?"new/nested/program":"program";save(path/program,version+" "+kind);
        pmu::GenerationSpec spec{version,"universal",{{program,hash(path/program)}}};
        if(kind=="runtime")spec.entrypoints={{"runtime_worker_armv7",program},{"manager_armv7",program}};
        else {save(path/"scripts/prepare.sh","new bootstrap");spec.files["scripts/prepare.sh"]=hash(path/"scripts/prepare.sh");spec.entrypoints={{"helper_armv7",program},{"bootstrap","scripts/prepare.sh"}};}
        const auto id=pmu::sha256Text(pmu::generationSpecToJson(spec).dump()+'\n');return {path,spec,key.proof(id,kind)};
    }
    pmu::VersionStore store(bool isHelper){return {isHelper?helper/".pmu-versions":source/"autorun/runtime/.pmu-versions",geteuid(),hash,{},true,[this,isHelper](const auto& id,const auto&,const auto& proof){pmu::verifyAuthorization(key.publicKey(),isHelper?"helper":"runtime",id,proof);},isHelper?"":"armv7"};}
    pmu::MigrationSelection migrate(const pmu::VersionStore::Checkpoint& point={}){return pmu::activateMigration(source,helper,identity,"armv7",key.publicKey(),runtimePayload,helperPayload,{},point);}
    void preserved(){check(read(source/"plugin-history.json")=="history"&&read(source/"local-plugins/keep")=="user package"&&read(helper/"controller.ini")=="controller configuration","migration preserves user inventory, local packages and controller configuration");}
};
}
int main(){
    char pattern[]="/tmp/pmu-migration-test.XXXXXX";const auto made=mkdtemp(pattern);if(!made)return 1;const fs::path root=made;
    try{
        {
            Fixture f(root/"normal");auto result=f.migrate();auto runtime=f.store(false),helper=f.store(true);
            check(runtime.current()->id==result.runtime&&helper.current()->id==result.helper,"both complete independent payloads activate");
            check(runtime.current()->spec.version=="4.0.0"&&runtime.current()->spec.entrypoints.at("manager_armv7")=="new/nested/program","skipped versions and changed layout need no old file-name list");
            check(fs::exists(runtime.current()->directory/"new/nested/program")&&!fs::exists(runtime.current()->directory/"program"),"removed owned files are absent from the new payload");
            check(runtime.boot()->id==result.runtime&&helper.boot()->id==result.helper,"both selected versions receive a first startup attempt");runtime.confirm(result.runtime);helper.confirm(result.helper);f.preserved();
        }
        {
            Fixture f(root/"usb-only");const auto result=pmu::activateMigration(f.source,{},f.identity,"armv7",f.key.publicKey(),f.runtimePayload,{});check(!result.runtime.empty()&&result.helper.empty(),"USB operation on 3.9 needs no controller or secondary SD");f.preserved();
        }
        for(const std::string damage:{"runtime-signature","helper-signature","runtime-file","helper-file","identity","architecture","missing-helper","pending"}){
            Fixture f(root/damage);
            if(damage=="runtime-signature")f.runtimePayload.authorization="{}";
            if(damage=="helper-signature")f.helperPayload.authorization="{}";
            if(damage=="runtime-file")save(f.runtimePayload.directory/"new/nested/program","damaged");
            if(damage=="helper-file")save(f.helperPayload.directory/"new/nested/program","damaged");
            if(damage=="identity")f.identity+='1';
            if(damage=="architecture")f.runtimePayload.specification.architecture="aarch64";
            if(damage=="pending"){const auto result=f.migrate();(void)result;}
            if(damage=="missing-helper")rejects([&]{pmu::activateMigration(f.source,f.helper,f.identity,"armv7",f.key.publicKey(),f.runtimePayload,{});},"an installed helper requires its approved update payload");
            else rejects([&]{f.migrate();},"unsafe or unconfirmed migration is refused");
            if(damage!="pending"){check(f.store(false).current()->id==f.firstRuntime&&f.store(true).current()->id==f.firstHelper,"all payloads must verify before either selection changes");}
            f.preserved();
        }
        {
            Fixture f(root/"source-replaced");rejects([&]{f.migrate([&](const auto& point){if(point=="migration-staged"){fs::rename(f.source,f.root/"retained");fs::create_directory(f.source);}});},"source replacement after staging prevents activation");check(fs::is_empty(f.source),"replacement source receives no writes");check(f.store(true).current()->id==f.firstHelper,"replacement source cannot activate its helper");
        }
        std::vector<std::string> points={"migration-staged","migration-helper-active","migration-runtime-active"};
        for(const auto& kind:{"runtime","helper"})for(const auto& point:{"selection-synced:healthy.json","selection-published:healthy.json","selection-durable:healthy.json","selection-synced:active.json","selection-published:active.json","selection-durable:active.json"})points.push_back(std::string(kind)+":"+point);
        for(const auto& point:points){
            Fixture f(root/("interrupted-"+std::to_string(checks)));const auto child=fork();check(child>=0,"fork migration interruption");
            if(!child){try{f.migrate([&](const auto& name){if(name==point)kill(getpid(),SIGKILL);});}catch(...){_exit(90);}_exit(91);}
            int status=0;check(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,"migration interrupted at its actual durable boundary");
            auto runtime=f.store(false),helper=f.store(true);const auto r=runtime.boot(),h=helper.boot();check(r&&h,"interrupted multi-store migration retains complete authenticated versions");
            check((r->id==f.firstRuntime||r->spec.version=="4.0.0")&&(h->id==f.firstHelper||h->spec.version=="4.0.0"),"recovery uses only the old or approved new versions");
            if(r->pending)check(runtime.boot()->id==f.firstRuntime,"unconfirmed runtime recovers the working version");
            if(h->pending)check(helper.boot()->id==f.firstHelper,"unconfirmed helper recovers the working version");
            f.preserved();
        }
        fs::remove_all(root);std::cout<<"PASS: "<<checks<<" update-migration complete-payload and interrupted activation checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
