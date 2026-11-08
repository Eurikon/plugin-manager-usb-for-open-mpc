#include "catalog_view.hpp"
#include <iostream>
#include <stdexcept>

static int checks=0;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);++checks;std::cout<<"PASS: "<<text<<'\n';}
int main(){try{
    using Json=nlohmann::json;
    for(const int themeLeft:{850,876,900}){pmu::CatalogTabs tabs{themeLeft-pmu::CatalogTabs::gap};check(tabs.left(0)==28&&tabs.right(3)+12==themeLeft,"tab group meets the theme button with the standard gap");for(int i=0;i<4;++i){check(tabs.hit(tabs.left(i))==i&&tabs.hit(tabs.right(i)-1)==i&&tabs.hit(tabs.right(i))==-1,"tab hit regions exclude button gaps");if(i)check(tabs.left(i)-tabs.right(i-1)==12,"all catalog tab gaps match Theme and Return");}}

    const std::vector<Json> source={{{"id","a"},{"name","alpha"},{"kind","instrument"},{"summary","Analog bass"},{"author","Example"},{"version","2.0.0"},{"installed_version","1.0.0"},{"update_available",true}},{{"id","z"},{"name","Zulu"},{"kind","effect"},{"summary","Delay"},{"author","Other"},{"blocked_reason","OS 2 only"}},{{"id","b"},{"name","Beta"},{"kind","instrument"},{"available_for_arch",false}}};
    pmu::BrowseOptions options;check(pmu::browsePlugins(source,options)[0].at("id")=="a","default sorting ignores ASCII capitalization");
    options.query="ANALOG example";auto result=pmu::browsePlugins(source,options);check(result.size()==1&&result[0].at("id")=="a","search matches all words across names, authors and descriptions");
    options.query="missing";check(pmu::browsePlugins(source,options).empty(),"unmatched search returns an empty result");options.query.clear();
    options.kind="effect";check(pmu::browsePlugins(source,options).size()==1,"type filtering selects effects");options.kind="instrument";options.updatesOnly=true;result=pmu::browsePlugins(source,options);check(result.size()==1&&result[0].at("version")=="2.0.0"&&result[0].at("installed_version")=="1.0.0","combined filters preserve available and installed version identities");
    options={};options.compatibleOnly=true;check(pmu::browsePlugins(source,options).size()==1,"compatibility filtering removes blocked and wrong-architecture packages");
    options={};options.order=pmu::BrowseOptions::Order::Reverse;check(pmu::browsePlugins(source,options)[0].at("id")=="z","reverse sort orders names from Z to A");
    options.order=pmu::BrowseOptions::Order::Updates;check(pmu::browsePlugins(source,options)[0].at("id")=="a","update-first sort prioritizes an available update");
    check(source[0].at("name")=="alpha"&&source[1].at("id")=="z","browsing does not mutate source records");
    std::map<std::string,pmu::BrowseOptions> tabs;tabs["catalog"].kind="effect";tabs["catalog"].updatesOnly=true;tabs["catalog"].order=pmu::BrowseOptions::Order::Reverse;tabs["catalog"].query="private search";tabs["local"].compatibleOnly=true;tabs["local"].order=pmu::BrowseOptions::Order::Updates;
    const auto saved=pmu::encodeBrowseSettings(tabs);const auto restored=pmu::decodeBrowseSettings(saved);
    check(restored.at("catalog").kind=="effect"&&restored.at("catalog").updatesOnly&&restored.at("catalog").order==pmu::BrowseOptions::Order::Reverse,"filter and sort settings roundtrip");
    check(restored.at("local").compatibleOnly&&restored.at("local").order==pmu::BrowseOptions::Order::Updates&&!restored.at("local").updatesOnly,"saved settings remain separate by tab");
    check(restored.at("catalog").query.empty()&&saved.dump().find("private search")==std::string::npos,"search text is excluded from persistent settings");
    check(pmu::decodeBrowseSettings(Json::object()).empty(),"missing saved tabs retain default behavior");
    for(const std::string defect:{"sort","kind","updates_only","tab","query"}){auto invalid=saved;if(defect=="sort")invalid["catalog"]["sort"]="other";else if(defect=="kind")invalid["catalog"]["kind"]="other";else if(defect=="updates_only")invalid["catalog"]["updates_only"]="yes";else if(defect=="tab")invalid["other"]=invalid["catalog"];else invalid["catalog"]["query"]="unexpected";bool rejected=false;try{pmu::decodeBrowseSettings(invalid);}catch(const std::exception&){rejected=true;}check(rejected,("invalid persistent "+defect+" is rejected").c_str());}
    std::string query="Aµ";pmu::eraseSearchCharacter(query);check(query=="A","backspace removes one complete UTF-8 character");pmu::eraseSearchCharacter(query);pmu::eraseSearchCharacter(query);check(query.empty(),"backspace handles the final character and empty input");
    check(pmu::wheelCard(-1,2,15,1)==12&&pmu::wheelCard(-1,2,15,-1)==14,"wheel starts within the currently displayed page");
    check(pmu::wheelCard(5,0,15,1)==6&&pmu::wheelCard(6,1,15,-1)==5,"wheel crosses pages in both directions");
    check(pmu::wheelCard(0,0,15,-127)==0&&pmu::wheelCard(14,2,15,127)==14,"wheel stops at catalog boundaries");
    check(pmu::wheelCard(-1,0,0,1)==-1&&pmu::wheelCard(-1,1,15,0)==-1,"empty lists and neutral wheel events retain selection");
    pmu::CardTap tap;
    check(!tap.open(0,100)&&tap.open(0,200),"two taps on the same card open details");
    check(!tap.open(0,250),"a third tap starts a new pair");
    tap.clear();check(!tap.open(0,300)&&!tap.open(1,350),"different cards cannot form a double tap");
    tap.clear();check(!tap.open(1,400)&&!tap.open(1,851),"slow repeated taps only select a card");
    tap.clear();check(!tap.open(1,900),"an intervening action clears the tap pair");
    tap.clear();check(!tap.open(2,UINT32_MAX-30)&&tap.open(2,20),"double tap timing survives timer wraparound");
    std::cout<<checks<<" catalog view checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
