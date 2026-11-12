#include "child_process.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace thumbgen::detail;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static PipeLine await(ChildProcess& child,const std::shared_ptr<ProcessWakeup>& wakeup) {
    const auto deadline=Clock::now()+3s;
    for(;;) {
        const auto observed=wakeup->sequence();auto result=child.read(Clock::now());
        if(result.state!=PipeLine::State::timeout)return result;
        require(wakeup->wait(observed,deadline),"child notification lost or deadline exceeded");
    }
}
static int test_main(const std::vector<std::string>& args){try {
    require(args.size()==2,"notification test requires process helper executable");const auto exe=std::filesystem::absolute(std::filesystem::u8path(args[1])).u8string();
    auto wakeup=std::make_shared<ProcessWakeup>();
    auto observed=wakeup->sequence();wakeup->notify();
    require(wakeup->wait(observed,Clock::now()),"notification before wait was lost");
    require(!wakeup->wait(wakeup->sequence(),Clock::now()),"empty predicate passed on expired deadline");
    // A publisher can run after the queue scan and before the wait acquires its lock.
    for(int i=0;i<100;++i) {
        observed=wakeup->sequence();std::promise<void> scanned;auto ready=scanned.get_future();
        std::thread publisher([&]{ready.wait();wakeup->notify();});scanned.set_value();
        const bool received=wakeup->wait(observed,Clock::now()+3s);publisher.join();require(received,"scan/wait race lost wakeup");
    }
    {
        ChildProcess echo(exe,{"--echo"},{},wakeup);
        // A wakeup without a frame must not be mistaken for a protocol message.
        observed=wakeup->sequence();wakeup->notify();require(wakeup->wait(observed,Clock::now()),"empty notification lost");
        require(echo.read(Clock::now()).state==PipeLine::State::timeout,"empty wakeup invented a frame");
        for(int i=0;i<100;++i){echo.send(std::to_string(i));auto r=await(echo,wakeup);require(r.state==PipeLine::State::line && r.text==std::to_string(i),"echo frame lost");}
        observed=wakeup->sequence();echo.stop();require(wakeup->wait(observed,Clock::now()),"stop did not notify");
    }
    {
        std::vector<std::unique_ptr<ChildProcess>> children;std::vector<bool> done(8,false);
        for(int i=0;i<8;++i){children.emplace_back(std::make_unique<ChildProcess>(exe,std::vector<std::string>{"--echo"},PipeLimits{},wakeup));children.back()->send(std::to_string(i));}
        const auto deadline=Clock::now()+3s;int remaining=8;
        while(remaining) {
            observed=wakeup->sequence();
            for(int i=0;i<8;++i)if(!done[i]){auto r=children[i]->read(Clock::now());if(r.state==PipeLine::State::line){require(r.text==std::to_string(i),"crossed child frames");done[i]=true;--remaining;}else require(r.state==PipeLine::State::timeout,"child failed");}
            if(remaining)require(wakeup->wait(observed,deadline),"multi-child notification lost");
        }
    }
    for(const auto& mode:{"--args","--partial","--flood","--queue"}) {
        PipeLimits limits;if(std::string(mode)=="--flood")limits.line_bytes=64;if(std::string(mode)=="--queue")limits.queued_lines=1;
        ChildProcess child(exe,{mode},limits,wakeup);
        if(std::string(mode)=="--queue") {
            // Drain notifications only, allowing the bounded queue to fill.
            const auto deadline=Clock::now()+3s;
            while(child.pipe_fault().empty()){observed=wakeup->sequence();if(!child.pipe_fault().empty())break;require(wakeup->wait(observed,deadline),"queue overflow did not notify");}
        }
        auto r=await(child,wakeup);require(r.state==(std::string(mode)=="--args"?PipeLine::State::eof:PipeLine::State::fault),"EOF/fault notification incorrect");
    }
    {
        ChildProcess quiet(exe,{"--sleep"},{},wakeup);observed=wakeup->sequence();
        require(!wakeup->wait(observed,Clock::now()+20ms),"quiet child generated a false state transition");
    }
    {
        auto owner=std::make_shared<ProcessWakeup>();std::weak_ptr<ProcessWakeup> weak=owner;
        {ChildProcess child(exe,{"--echo"},{},owner);owner.reset();require(!weak.expired(),"child lost notifier ownership");child.send("owned");require(child.read(Clock::now()+3s).text=="owned","notifier destruction race");}
        require(weak.expired(),"notifier leaked after pumps joined");
    }
    for(int i=0;i<50;++i){auto owner=std::make_shared<ProcessWakeup>();ChildProcess child(exe,{"--queue"},{},owner);owner.reset();child.stop();}
    std::cout<<"notification: pre-wait, scan races, empty wakes, concurrent children, EOF/fault, deadlines, stop and lifetime passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(std::size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),n,nullptr,nullptr);value.pop_back();args.push_back(std::move(value));}
    return test_main(args);
}
#else
int main(int argc,char** argv){return test_main(std::vector<std::string>(argv,argv+argc));}
#endif
