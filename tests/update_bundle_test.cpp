#include "update_bundle.hpp"
#include "crypto.hpp"
#include "signing_fixture.hpp"
#include "checksum.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
size_t checks=0;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class Function> void rejects(Function function,const char* message){try{function();}catch(const std::exception&){++checks;return;}throw std::runtime_error(message);}
std::string read(const fs::path& path){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Cannot read test input");return {std::istreambuf_iterator<char>(input),{}};}
void write(const fs::path& path,const std::string& text){std::ofstream output(path,std::ios::binary);output<<text;if(!output)throw std::runtime_error("Cannot write test input");}
struct Entry {std::string name,data;mode_t mode=AE_IFREG;};
void zip(const fs::path& path,const std::vector<Entry>& entries){
    auto* output=archive_write_new();if(!output)throw std::runtime_error("Cannot create ZIP fixture");
    try{
        check(archive_write_set_format_zip(output)==ARCHIVE_OK&&archive_write_open_filename(output,path.c_str())==ARCHIVE_OK,"ZIP fixture opened");
        for(const auto& value:entries){auto* entry=archive_entry_new();if(!entry)throw std::runtime_error("Cannot create ZIP entry");archive_entry_set_pathname(entry,value.name.c_str());archive_entry_set_filetype(entry,value.mode);archive_entry_set_perm(entry,0644);archive_entry_set_size(entry,value.mode==AE_IFREG?value.data.size():0);if(value.mode==AE_IFLNK)archive_entry_set_symlink(entry,value.data.c_str());
            const int status=archive_write_header(output,entry);archive_entry_free(entry);if(status!=ARCHIVE_OK)throw std::runtime_error("Cannot write ZIP header");if(value.mode==AE_IFREG&&archive_write_data(output,value.data.data(),value.data.size())!=ssize_t(value.data.size()))throw std::runtime_error("Cannot write ZIP payload");
        }
        check(archive_write_close(output)==ARCHIVE_OK,"ZIP fixture finished");archive_write_free(output);
    }catch(...){archive_write_free(output);throw;}
}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    std::string pattern="/tmp/pmu-bundle-test.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);const auto made=mkdtemp(name.data());if(!made)return 1;const fs::path root=made;
    try{
#if defined(__aarch64__)
        const std::string architecture="aarch64",other="armv7";
#else
        const std::string architecture="armv7",other="aarch64";
#endif
        const std::string program=read(fs::canonical(argv[1])),workerPath="future-layout/v9/migrate",dataPath="new-name/assets/ready.txt",data="ready\n";write(root/"data",data);
        pmu::GenerationSpec spec{"9.8.7",architecture,{{workerPath,pmu::fileSha256(fs::canonical(argv[1]))},{dataPath,pmu::fileSha256(root/"data")}},{{"update_worker_"+architecture,workerPath}}};
        pmu::test::SigningFixture key;const auto publisher=key.publicKey();auto signedEnvelope=[&](const auto& descriptor){return pmu::updateEnvelope(descriptor,key.proof(pmu::sha256Text(pmu::generationSpecToJson(descriptor).dump()+'\n'),"update"));};
        auto envelope=signedEnvelope(spec);check(pmu::generationSpecFromJson(envelope.at("generation")).files==spec.files,"generation metadata has one validated codec");
        std::vector<Entry> original={{"PMU-UPDATE.json",envelope.dump()},{"future-layout/","",AE_IFDIR},{workerPath,program},{dataPath,data}};
        const auto archive=root/"update.zip",output=root/"unpacked";zip(archive,original);int progress=0;
        auto unpack=[&](const fs::path& destination,const std::string& sha,const std::string& arch,const std::string& version){return pmu::unpackUpdateBundle(archive,sha,fs::file_size(archive),destination,arch,version,publisher,{{},[&](float fraction){check(fraction>=0&&fraction<=1,"bundle progress is bounded");++progress;}});};
        auto result=unpack(output,pmu::fileSha256(archive),architecture,spec.version);check(result.spec.files==spec.files&&result.worker.executable==output/workerPath&&progress>0,"new layout selects its native worker without a file-name whitelist");check(read(output/dataPath)==data,"all inventoried payload files are extracted intact");
        check(pmu::runIncomingWorker(result.worker,root,{{"operation","apply"},{"echo","bundle-roundtrip"}}).at("echo")=="bundle-roundtrip","download identity, envelope, worker verification and protocol work together");
        const auto sentinel=output/"user-file";write(sentinel,"preserved");rejects([&]{unpack(output,pmu::fileSha256(archive),architecture,spec.version);},"existing staging folder is refused");check(read(sentinel)=="preserved","preexisting files remain untouched");fs::remove_all(output);
        rejects([&]{unpack(output,std::string(64,'a'),architecture,spec.version);},"outer ZIP checksum mismatch is refused");check(!fs::exists(output),"outer mismatch precedes staging writes");
        rejects([&]{unpack(output,pmu::fileSha256(archive),other,spec.version);},"wrong device architecture is refused");rejects([&]{unpack(output,pmu::fileSha256(archive),architecture,"9.8.8");},"release version must match the verified lookup");
        auto failure=[&](std::vector<Entry> entries,const char* label){zip(archive,entries);rejects([&]{unpack(output,pmu::fileSha256(archive),architecture,spec.version);},label);check(!fs::exists(output),"failed extraction removes only its own staging data");};
        auto entries=original;entries.back().data="wrong";failure(entries,"payload checksum mismatch is refused and cleaned");
        entries=original;entries.push_back({"unexpected.txt","unlisted"});failure(entries,"uninventoried files are refused");
        entries=original;entries.push_back(original.back());failure(entries,"duplicate ZIP paths are refused");
        entries=original;entries.push_back({"../escaped","invalid"});failure(entries,"path traversal is refused");check(!fs::exists(root/"escaped"),"traversal creates no outside file");
        entries=original;entries.push_back({"unlisted-link","/etc/passwd",AE_IFLNK});failure(entries,"ZIP symlinks are refused");
        entries=original;entries.pop_back();failure(entries,"missing declared payload is refused");
        entries=original;entries[0].data="{\"protocol\":2,\"protocol\":1,\"product\":\"plugin-manager-usb\",\"generation\":"+envelope.at("generation").dump()+"}";failure(entries,"duplicate envelope keys are refused");
        entries=original;auto future=envelope;future["protocol"]=2;entries[0].data=future.dump();failure(entries,"unsupported base protocol is refused");
        entries=original;entries[0].data=std::string(129*1024,'x');failure(entries,"metadata is bounded before parsing");
        entries=original;entries[0].data="{\"future\":"+std::string(24,'[')+"0"+std::string(24,']')+",\"protocol\":1,\"product\":\"plugin-manager-usb\",\"generation\":"+envelope.at("generation").dump()+"}";failure(entries,"metadata nesting is bounded");
        entries=original;auto extension=envelope;extension["future"]="unsigned hint";entries[0].data=extension.dump();failure(entries,"extra protocol fields cannot carry unsigned migration instructions");
        entries=original;auto futureSpec=spec;const std::string futurePath="new-format/migration-v9.json",futureText=Json{{"migration_hint","interpreted only by the incoming worker"}}.dump();futureSpec.files[futurePath]=pmu::sha256Text(futureText);entries[0].data=signedEnvelope(futureSpec).dump();entries.push_back({futurePath,futureText});zip(archive,entries);result=unpack(output,pmu::fileSha256(archive),architecture,spec.version);check(read(output/futurePath)==futureText,"opaque future metadata is delivered as an inventoried signed file");fs::remove_all(output);
        entries=original;auto forged=spec;forged.files[dataPath]=pmu::sha256Text("replaced");entries[0].data=pmu::updateEnvelope(forged,envelope.at("authorization").get<std::string>()).dump();entries.back().data="replaced";failure(entries,"rewriting the ZIP and all file checksums cannot forge publisher approval");
        auto missing=spec;missing.entrypoints.clear();rejects([&]{pmu::updateEnvelope(missing);},"a release must provide a worker for every declared architecture");
        auto self=spec;self.files["PMU-UPDATE.json"]=std::string(64,'0');rejects([&]{pmu::updateEnvelope(self);},"the envelope cannot hash itself");
        auto universal=spec;universal.architecture="universal";rejects([&]{pmu::updateEnvelope(universal);},"universal releases must declare both native workers");universal.entrypoints["update_worker_"+other]=workerPath;check(pmu::updateEnvelope(universal).at("generation").at("architecture")=="universal","the base envelope supports both generations");
        zip(archive,original);fs::create_symlink(archive,root/"linked.zip");rejects([&]{pmu::unpackUpdateBundle(root/"linked.zip",pmu::fileSha256(archive),fs::file_size(archive),output,architecture,spec.version,publisher);},"linked ZIP inputs are refused");
        fs::create_directory(root/"outside");fs::create_symlink(root/"outside",root/"linked-parent");rejects([&]{unpack(root/"linked-parent"/"unpacked",pmu::fileSha256(archive),architecture,spec.version);},"linked staging ancestors are refused");check(fs::is_empty(root/"outside"),"linked staging targets stay untouched");
        check(!fs::exists(output),"negative cases leave no partial published update");fs::remove_all(root);std::cout<<"PASS: "<<checks<<" update-bundle integrity, forward-compatibility and staging checks\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
