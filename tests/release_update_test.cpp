#include "backend.hpp"
#include "checksum.hpp"
#include "update_bundle.hpp"
#include <publisher_key.hpp>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static void save(const fs::path& path,const std::string& text){std::ofstream file(path);file<<text;if(!file)throw std::runtime_error("Cannot write release fixture");}
int main(int argc,char** argv){
    if(argc!=5)return 2;
    fs::path root;
    try{
        const auto archive=fs::canonical(argv[1]);const std::string version=argv[2],architecture=argv[3],digest=argv[4];
        char temporary[]="/tmp/pmu-final-release.XXXXXX";const auto made=mkdtemp(temporary);if(!made)throw std::runtime_error("Cannot create release fixture");root=made;
        for(const auto* name:{"source/autorun/runtime/.pmu-versions","session","synths","bin","worker"})fs::create_directories(root/name);
        save(root/"source/.mpc-manager-id","mpc-usb-manager-v1\n");save(root/"settings","<PROPERTIES/>\n");
        const auto settings=pmu::fileSha256(root/"settings");
        const std::string tag="v"+version,name="plugin-manager-usb-"+tag+".zip",base="https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/";
        const Json metadata={{"draft",false},{"prerelease",false},{"tag_name",tag},{"html_url",base+"tag/"+tag},{"assets",Json::array({{{"name",name},{"state","uploaded"},{"size",fs::file_size(archive)},{"digest","sha256:"+digest},{"browser_download_url",base+"download/"+tag+"/"+name}}})}};
        save(root/"response.json",metadata.dump());
        save(root/"bin/wget","#!/bin/sh\nset -eu\noutput=''\nurl=''\nwhile [ \"$#\" -gt 0 ]; do case \"$1\" in -O) shift; output=$1 ;; https://*) url=$1 ;; esac; shift; done\ncase \"$url\" in https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest) cp \"$PMU_TEST_METADATA\" \"$output\" ;; \"$PMU_TEST_ASSET_URL\") cp \"$PMU_TEST_ARCHIVE\" \"$output\" ;; *) exit 7 ;; esac\n");
        if(chmod((root/"bin/wget").c_str(),0700))throw std::runtime_error("Cannot enable release fixture downloader");
        const char* previous=getenv("PATH");const auto path=(root/"bin").string()+":"+(previous?previous:"");
        if(setenv("PATH",path.c_str(),1)||setenv("PMU_TEST_METADATA",(root/"response.json").c_str(),1)||setenv("PMU_TEST_ARCHIVE",archive.c_str(),1)||setenv("PMU_TEST_ASSET_URL",(base+"download/"+tag+"/"+name).c_str(),1))throw std::runtime_error("Cannot configure release fixture");
        Manager manager(root/"source",root/"session",root/"synths",root/"settings",architecture);
        const auto checked=manager.checkManagerUpdate();
        if(!checked.at("update_available").get<bool>()||!checked.at("downloadable").get<bool>())throw std::runtime_error("Baseline updater did not offer the target release");
        const auto staged=manager.stageManagerUpdate(checked);
        const auto verified=pmu::verifyStagedUpdate(staged.at("bundle_root").get<std::string>(),architecture,version,pmu::publisherKey);
        const auto hello=pmu::runIncomingWorker(verified.worker,root/"worker",{{"operation","hello"}});
        if(hello.value("protocol",0)!=1||hello.value("product","")!="plugin-manager-usb")throw std::runtime_error("Release worker did not complete its protocol handshake");
        if(pmu::fileSha256(root/"settings")!=settings||fs::exists(root/"source/PMU-UPDATE.json"))throw std::runtime_error("Release check changed its simulated installation");
        std::cout<<Json({{"schema",1},{"baseline_version",checked.at("current_version")},{"baseline_development",checked.value("development",false)},{"baseline_development_reported",checked.contains("development")},{"version",version},{"architecture",architecture},{"archive_sha256",digest},{"download_checked",true},{"signed_archive_checked",true},{"native_worker_ready",true},{"installation_unchanged",true}}).dump()<<'\n';
        fs::remove_all(root);return 0;
    }catch(const std::exception& error){if(!root.empty()){std::error_code ignored;fs::remove_all(root,ignored);}std::cerr<<error.what()<<'\n';return 1;}
}
