#include "child_process.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace fs=std::filesystem;
using namespace thumbgen::detail;
using Clock=std::chrono::steady_clock;
using State=PipeLine::State;
using namespace std::chrono_literals;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static std::string hex(const std::string& text) {
    std::string result;for(unsigned char c:text){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;
}
static int wait_exit(ChildProcess& child) {
    auto deadline=Clock::now()+3s;
    while(Clock::now()<deadline){auto code=child.exit_code();if(code)return *code;std::this_thread::sleep_for(5ms);}
    throw std::runtime_error("child did not exit");
}
static std::string line(ChildProcess& child) {
    auto value=child.read(Clock::now()+3s);require(value.state==State::line,"expected line, got: "+value.text);return value.text;
}
static bool running(unsigned long pid) {
#ifdef _WIN32
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));if(!process)return false;
    bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);return alive;
#else
    std::ifstream stat("/proc/"+std::to_string(pid)+"/stat");std::string value;std::getline(stat,value);
    auto end=value.rfind(')');return end!=std::string::npos && end+2<value.size() && value[end+2]!='Z';
#endif
}
static int run(const std::vector<std::string>& args) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
#else
    struct rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
#endif
    const auto mode=args.size()>1?args[1]:std::string{};
    if(mode=="--sleep"){std::this_thread::sleep_for(15s);return 0;}
    if(mode=="--crash") {
#ifdef _WIN32
        RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
#else
        std::raise(SIGSEGV);
#endif
        return 19;
    }
    if(mode=="--partial"){std::cout<<"unfinished"<<std::flush;return 0;}
    if(mode=="--flood"){std::cout<<std::string(20000,'x')<<std::flush;return 0;}
    if(mode=="--queue"){for(int i=0;i<20;++i)std::cout<<i<<'\n';std::cout<<std::flush;return 0;}
    if(mode=="--stderr"){std::cerr<<std::string(256*1024,'e')<<"tail"<<std::flush;std::cout<<"done\n"<<std::flush;return 0;}
    if(mode=="--args"){for(std::size_t i=2;i<args.size();++i)std::cout<<hex(args[i])<<'\n';return 0;}
    if(mode=="--echo"){std::string text;while(std::getline(std::cin,text)){if(text=="quit")return 0;std::cout<<text<<'\n'<<std::flush;}return 0;}
    if(mode=="--tree") {
#ifdef _WIN32
        auto executable=fs::u8path(args[0]).native();auto command=L"\""+executable+L"\" --sleep";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=0,"helper grandchild spawn failed");
        auto pid=child.dwProcessId;CloseHandle(child.hProcess);CloseHandle(child.hThread);
#else
        auto pid=fork();require(pid>=0,"helper fork failed");if(!pid){sleep(15);_exit(0);}
#endif
        std::cout<<pid<<'\n'<<std::flush;std::this_thread::sleep_for(15s);return 0;
    }
    try {
        auto root=fs::temp_directory_path()/fs::u8path("thumbgen-child-"+std::to_string(Clock::now().time_since_epoch().count())+u8" 汉字");
        require(fs::create_directory(root),"cannot reserve test directory");
        auto executable=root/fs::u8path(u8"worker helper 空格"+fs::u8path(args[0]).extension().u8string());
        struct Cleanup {fs::path file,dir;~Cleanup(){std::error_code ec;fs::remove(file,ec);fs::remove(dir,ec);}} cleanup{executable,root};
        fs::copy_file(fs::absolute(fs::u8path(args[0])),executable);
        auto exe=executable.u8string();
        {
            std::vector<std::string> values{"","plain","two words","embedded\"quote","end\\","\\\"\\",u8"中文 😀","a&b;$()"};
            auto parameters=values;parameters.insert(parameters.begin(),"--args");ChildProcess child(exe,parameters);
            for(const auto& expected:values)require(line(child)==hex(expected),"argument quoting/UTF-8 mismatch");
            require(wait_exit(child)==0,"args helper failed");
        }
        ChildProcess sibling(exe,{"--echo"});sibling.send("still-alive");require(line(sibling)=="still-alive","sibling failed");
        {
            ChildProcess crash(exe,{"--crash"});require(wait_exit(crash)!=0,"native crash reported success");
        }
        {
            ChildProcess after(exe,{"--echo"});after.send("after-crash");require(line(after)=="after-crash","normal worker after crash failed");
            after.send("quit");require(wait_exit(after)==0,"normal worker exit failed");
        }
        {
            ChildProcess tree(exe,{"--tree"});auto descendant=std::stoul(line(tree));require(running(descendant),"grandchild was not running");
            tree.stop();auto deadline=Clock::now()+3s;
            while(running(descendant) && Clock::now()<deadline)std::this_thread::sleep_for(5ms);
            require(!running(descendant),"owned grandchild survived stop");
            require(!sibling.exit_code(),"stopping owned group killed an unrelated sibling");
            sibling.send("survived-stop");require(line(sibling)=="survived-stop","sibling pipe closed");
        }
        {
            ChildProcess blocked(exe,{"--sleep"});require(blocked.read(Clock::now()+20ms).state==State::timeout,"deadline was ignored");
            blocked.send(std::string(1024*1024,'x'));std::this_thread::sleep_for(20ms);
            auto start=Clock::now();blocked.stop();require(Clock::now()-start<3s,"stop hung on blocked stdin");
            require(blocked.exit_code().has_value(),"stopped child was not reaped");
        }
        {
            PipeLimits limits;limits.stderr_bytes=128;ChildProcess noisy(exe,{"--stderr"},limits);
            require(line(noisy)=="done","stderr backpressure blocked stdout");require(wait_exit(noisy)==0,"noisy helper failed");noisy.stop();
            require(noisy.stderr_truncated() && noisy.stderr_text().size()==128 && noisy.stderr_text().substr(124)=="tail","stderr cap/tail mismatch");
        }
        {
            ChildProcess partial(exe,{"--partial"});require(partial.read(Clock::now()+3s).state==State::fault,"truncated frame accepted");
            PipeLimits limits;limits.line_bytes=128;ChildProcess flood(exe,{"--flood"},limits);
            require(flood.read(Clock::now()+3s).state==State::fault,"oversized frame accepted");
        }
        {
            PipeLimits limits;limits.queued_lines=2;ChildProcess queued(exe,{"--queue"},limits);
            auto deadline=Clock::now()+3s;
            while(queued.pipe_fault().empty() && Clock::now()<deadline)std::this_thread::sleep_for(5ms);
            require(queued.read(Clock::now()).state==State::fault,"unbounded stdout queue");
        }
        {
            PipeLimits limits;limits.line_bytes=65536;limits.queued_bytes=131072;limits.queued_lines=2;
            ChildProcess blocked(exe,{"--sleep"},limits);bool rejected=false;
            for(int i=0;i<20;++i)try {blocked.send(std::string(65536,'x'));}catch(const std::runtime_error&){rejected=true;break;}
            require(rejected,"unbounded stdin queue");blocked.stop();
        }
        bool rejected=false;
        try {ChildProcess missing(exe+".missing",{});rejected=wait_exit(missing)==127;}
        catch(const std::system_error&){rejected=true;}
        // Older glibc may report post-fork exec failure as child exit 127.
        require(rejected,"missing executable did not fail at spawn or exec");
#ifndef _WIN32
        {
            struct sigaction previous{},ignored{};ignored.sa_handler=SIG_IGN;sigemptyset(&ignored.sa_mask);
            require(sigaction(SIGCHLD,&ignored,&previous)==0,"cannot set test signal disposition");
            struct Restore {struct sigaction action;~Restore(){sigaction(SIGCHLD,&action,nullptr);}} restore{previous};
            bool refused=false;try {ChildProcess unsafe(exe,{"--sleep"});}catch(const std::runtime_error&){refused=true;}
            require(refused,"SIGCHLD auto-reaper ownership violation accepted");
        }
#endif
        sibling.send("quit");require(wait_exit(sibling)==0,"sibling did not exit normally");sibling.stop();
        std::cout<<"process: arguments/UTF-8, crash recovery, group ownership, blocked pipes, bounds and deadlines passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string arg(std::size_t(n),'\0');
        WideCharToMultiByte(CP_UTF8,0,argv[i],-1,arg.data(),n,nullptr,nullptr);arg.pop_back();args.push_back(std::move(arg));
    }
    return run(args);
}
#else
int main(int argc,char** argv){return run(std::vector<std::string>(argv,argv+argc));}
#endif
