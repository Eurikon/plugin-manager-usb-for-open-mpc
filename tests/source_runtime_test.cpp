#include "source_runtime.hpp"
#include "crypto.hpp"
#include "checksum.hpp"
#include "metadata.hpp"
#include "signing_fixture.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
void save(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path);output<<text;output.close();if(!output||chmod(path.c_str(),0600))throw std::runtime_error("Cannot write source fixture");}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Cannot read source fixture");return {std::istreambuf_iterator<char>(input),{}};}
}
int main(){
    std::string pattern="/tmp/pmu-source-runtime.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);const auto made=mkdtemp(name.data());if(!made)return 1;const fs::path root=made;
    try{
#if defined(__aarch64__)
        const std::string architecture="aarch64";
        constexpr unsigned machine=183;
#else
        const std::string architecture="armv7";
        constexpr unsigned machine=40;
#endif
        pmu::test::SigningFixture key,other;const auto publisher=key.publicKey();const auto source=root/"USB",input=root/"input";fs::create_directories(input);save(source/".mpc-manager-id","mpc-usb-manager-v1\n");save(source/"manager-preferences.json","preserved");
        {std::ifstream self("/proc/self/exe",std::ios::binary);std::array<unsigned char,64> header{};self.read(reinterpret_cast<char*>(header.data()),header.size());check(self.gcount()==64&&header[0]==0x7f&&(unsigned(header[18])|(unsigned(header[19])<<8))==machine,"self executable is the target program rather than its emulation host");self.clear();self.seekg(0);std::ofstream copy(input/"manager",std::ios::binary);copy<<self.rdbuf();copy.close();check(bool(copy),"running manager image copied for startup binding fixture");}
        auto hash=[](const auto& path){return pmu::fileSha256(path);};const auto location=source/"autorun/runtime/.pmu-versions";fs::create_directories(location.parent_path());pmu::VersionStore store(location,geteuid(),hash,{},true,{},architecture);
        pmu::GenerationSpec spec{"1.0.0",architecture,{{"manager",hash(input/"manager")}},{{"manager_"+architecture,"manager"},{"runtime_worker_"+architecture,"manager"}}};
        auto stage=[&](const auto& value){const auto id=pmu::sha256Text(pmu::generationSpecToJson(value).dump()+'\n');return store.stage(input,value,key.proof(id,"runtime"));};const auto first=stage(spec);store.seed(first);
        pmu::SourceRuntime runtime(source,architecture,publisher);const auto identity=runtime.identity();const auto initial=runtime.choose(false);check(initial.generation.id==first&&initial.identity==identity&&initial.worker.sha256==spec.files.at("manager"),"source selects its approved architecture-specific worker");
        spec.version="2.0.0";const auto second=stage(spec);store.activate(second);runtime.choose(false);runtime.choose(false);check(!pmu::parseMetadata(read(location/("active."+architecture+".json"))).at("attempted").get<bool>(),"silent preparation never consumes the pending GUI startup attempt");rejects([&]{runtime.confirm(second,identity);},"preparation cannot be reported as successful GUI startup");
        check(runtime.choose(true,identity).generation.id==second&&store.current()->pending,"actual opening durably marks the selected GUI attempt");runtime.confirm(second,identity);check(!store.current()->pending,"the matching running image confirms its first successful frame");
        rejects([&]{runtime.confirm(first,identity);},"old generation cannot acknowledge a newer selection");rejects([&]{runtime.confirm(second,"");},"startup confirmation requires its source identity");rejects([&]{runtime.choose(false,identity+"1");},"another source identity cannot select a payload");
        check(runtime.retained(first,identity).generation.id==first&&store.current()->id==second,"retained authenticated versions can be inspected without replacing the selected release");rejects([&]{runtime.retained(first,identity+"1");},"retained inspection still requires the exact source identity");
        spec.version="3.0.0";save(input/"manager","different manager image");spec.files["manager"]=hash(input/"manager");const auto third=stage(spec);store.activate(third);runtime.choose(true,identity);rejects([&]{runtime.confirm(third,identity);},"an old running GUI cannot confirm a different new binary");check(runtime.recover(third,identity).generation.id==second&&!store.current()->pending,"failed GUI startup explicitly restores the verified previous source version");rejects([&]{runtime.recover(third,identity);},"stale failure report cannot roll back a later selection");
        {pmu::SourceRuntime untrusted(source,architecture,other.publicKey());rejects([&]{untrusted.choose(false);},"another publisher cannot authorize source execution");}
        const auto secondary=root/"SD";save(secondary/".mpc-manager-id","mpc-usb-manager-v1\n");fs::create_directories(secondary/"autorun/runtime");pmu::VersionStore sd(secondary/"autorun/runtime/.pmu-versions",geteuid(),hash,{},true,{},architecture);const auto secondaryId=sd.stage(input,spec,key.proof(third,"runtime"));sd.seed(secondaryId);pmu::SourceRuntime secondaryRuntime(secondary,architecture,publisher);check(secondaryRuntime.choose(false).generation.id==third&&runtime.choose(false).generation.id==second,"USB and SD retain independent approved versions and selection records");
        fs::rename(source,root/"retained");fs::create_directory(source);rejects([&]{runtime.choose(false);},"removed and substituted source folders are refused");check(fs::is_empty(source),"replacement source receives no activation or health writes");rejects([&]{runtime.confirm(second,identity);},"health cannot be written through a substituted source path");fs::remove(source);fs::rename(root/"retained",source);check(runtime.choose(false,identity).generation.id==second,"retained original source remains intact");
        fs::remove(source/".mpc-manager-id");save(source/".mpc-manager-id","mpc-usb-manager-v1\n");rejects([&]{runtime.choose(false,identity);},"replacing the marker invalidates the original source binding");check(read(source/"manager-preferences.json")=="preserved","source activation and failure recovery preserve preferences");
        fs::remove_all(root);std::cout<<"PASS: "<<checks<<" source-runtime preparation, GUI health and media-binding checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
