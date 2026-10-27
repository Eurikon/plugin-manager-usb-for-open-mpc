#define main shortcutSupervisorMain
#include "../src/shortcut.cpp"
#undef main
#include <cassert>

int main(){
    unsigned checks=0;
    const auto parsed=[&](const std::string& text,const std::string& expected){
        std::string value="inherited";applyEnvironmentFile(text,value);assert(value==expected);++checks;
    };
    parsed("OTHER=value\n","inherited");
    parsed(" # comment\n; LD_PRELOAD=ignored\nnot-an-assignment\n","inherited");
    parsed("LD_PRELOAD= /first.so : /second.so \t\r\n","/first.so : /second.so");
    parsed("LD_PRELOAD=\"/first.so /second.so\"\n","/first.so /second.so");
    parsed("LD_PRELOAD=' /first.so ' \n"," /first.so ");
    parsed("LD_PRELOAD=\n","");
    parsed("LD_PRELOAD=/old.so\nLD_PRELOAD=/new.so\n","/new.so");
    parsed("LD_PRELOAD=/first.so\\\n /second.so\n","/first.so /second.so");
    parsed("LD_PRELOAD=\"/first.so\\\n /second.so\"\n","/first.so /second.so");
    parsed("OTHER='line\nLD_PRELOAD=/ignored.so'\n","inherited");
    parsed("LD_PRELOAD=$ORIGIN/$(literal)/`literal`\n","$ORIGIN/$(literal)/`literal`");
    parsed("LD_PRELOAD=\"\\$ORIGIN/\\q/\\\"literal\\\"\"\n","$ORIGIN/\\q/\"literal\"");
    parsed("LD_PRELOAD=/first.so\\ /second.so\n","/first.so /second.so");
    parsed("LD_PRELOAD=prefix'quote'\n","prefix'quote'");
    for(const std::string& text:{std::string("LD_PRELOAD=x\0y",14),std::string("LD_PRELOAD='unterminated")}){
        std::string value;bool failed=false;try{applyEnvironmentFile(text,value);}catch(const std::runtime_error&){failed=true;}assert(failed);++checks;
    }
    char pattern[]="/tmp/pmu-environment-test.XXXXXX";const char* created=mkdtemp(pattern);assert(created);const fs::path root=created;
    try{
        {std::ofstream file(root/"10-first.env");file<<"LD_PRELOAD=/first.so\n";}
        {std::ofstream file(root/"20 last.env");file<<"LD_PRELOAD='/last.so /final.so'\n";}
        std::string value="inherited";applyEnvironmentFiles(root.string()+"/*.env (ignore_errors=no)",value);assert(value=="/last.so /final.so");++checks;
        value="inherited";applyEnvironmentFiles((root/"20 last.env").string()+" (ignore_errors=no)",value);assert(value=="/last.so /final.so");++checks;
        applyEnvironmentFiles((root/"missing.env").string()+" (ignore_errors=yes)",value);assert(value=="/last.so /final.so");++checks;
        bool failed=false;try{applyEnvironmentFiles((root/"missing.env").string()+" (ignore_errors=no)",value);}catch(const std::runtime_error&){failed=true;}assert(failed);++checks;
        fs::create_directory(root/"directory.env");failed=false;try{applyEnvironmentFiles((root/"directory.env").string()+" (ignore_errors=no)",value);}catch(const std::runtime_error&){failed=true;}assert(failed);++checks;
    }catch(...){fs::remove_all(root);throw;}
    fs::remove_all(root);std::cout<<"PASS: "<<checks<<" environment parsing, precedence, literal-value and file validation checks\n";
}
