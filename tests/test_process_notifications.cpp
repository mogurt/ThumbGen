#include "child_process.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#if defined(THUMBGEN_TEST_HOOKS)
#include "isolated.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <atomic>
#include <random>
#endif
using namespace thumbgen::detail;
#if defined(THUMBGEN_TEST_HOOKS)
using namespace thumbgen;
#endif
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
#if defined(THUMBGEN_TEST_HOOKS)
namespace fs=std::filesystem;
static void noisy_image(const fs::path& path,int size) {
    OIIO::ImageSpec spec(size,size,3,OIIO::TypeDesc::FLOAT);
    std::mt19937 rng(12345);std::uniform_real_distribution<float> values(0.0f,1.0f);
    std::vector<float> pixels(size_t(size)*size*3);
    for(auto& pixel:pixels)pixel=values(rng);
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror().c_str());
    require(output->open(path.u8string(),spec),output->geterror().c_str());
    require(output->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),output->geterror().c_str());
    require(output->close(),output->geterror().c_str());
}
// Deterministic interleave at the real controller scan/wait boundary: the hook
// runs inside run_isolated when a lane scan found no progress, before the
// predicate wait. Landing a complete frame/notify cycle there must make the
// wait return immediately instead of sleeping past the notification.
static void controller_scan_hook(const std::vector<std::string>& args) {
    const auto helper=fs::absolute(fs::u8path(args[1])).u8string();
    const auto worker=fs::absolute(fs::u8path(args[0])).u8string();
    auto root=fs::temp_directory_path()/("thumbgen-notify-"+std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    Job job;job.id="notify-hook";job.input=(root/"source.exr").u8string();job.options.input_space="linear";
    noisy_image(fs::u8path(job.input),1280);job.outputs.push_back({(root/"out.png").u8string(),"png",64,64,90});
    std::atomic<size_t> fires{0};std::atomic<bool> roundtrip{false};
    std::atomic<long long> first_end{0},gap_us{-1};
    struct HookReset {~HookReset(){set_test_scan_hook();}} reset;
    set_test_scan_hook([&](const std::shared_ptr<ProcessWakeup>& wakeup){
        const auto n=fires.fetch_add(1)+1;
        if(n==1) {
            ChildProcess echo(helper,{"--echo"},{},wakeup);
            echo.send("hook-ping");auto r=await(echo,wakeup);
            require(r.state==PipeLine::State::line && r.text=="hook-ping","scan hook echo frame lost");
            roundtrip=true;echo.stop();
            first_end.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
        } else if(n==2) {
            gap_us.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-Clock::time_point(std::chrono::microseconds(first_end.load()))).count());
        }
    });
    Json records=Json::array();
    bool interrupted=run_isolated({job},worker,[&](const Json& record){records.push_back(record);});
    require(!interrupted,"notification hook batch reported interruption");
    require(records.size()==1 && records[0]["status"]=="success",("scan hook batch failed: "+(records.empty()?std::string("no record"):records[0].dump())).c_str());
    require(roundtrip,"controller never reached the no-progress scan/wait boundary");
    require(gap_us.load()>=0,"controller wait blocked past the interleaved notification");
    require(gap_us.load()<50000,"controller wait did not return promptly after the interleaved notification");
    fs::remove_all(root);
}
static void change_preview_mapping(const std::string& type,const std::string& task,Json& message) {
    if(type=="encoded" && task=="changed-cryptomatte") {
        message.at("encoded").at("base").at("cryptomatte").at("metadata_key")="cryptomatte/test456";
        return;
    }
    Json* record=nullptr;
    if(type=="encoded" && task=="changed-encoded")record=&message.at("encoded").at("base");
    if(type=="result" && task=="changed-result")record=&message.at("record");
    if(!record)return;
    // Removing ZBack gives a different but well-formed point-sample mapping.
    const Json channels={"A","Z"};
    record->at("deep").at("mapping").at("zback")="";
    record->at("selection").at("chosen").at("channels")=channels;
    record->at("selected_channels")=channels;
}
static void check_automatic_preview_admission(const std::string& worker,const fs::path& root) {
    OIIO::ImageSpec spec(1,1,3,OIIO::TypeDesc::FLOAT);
    spec.deep=true;spec.channelnames={"A","Z","ZBack"};spec.alpha_channel=0;spec.z_channel=1;
    spec.attribute("compression","zips");
    OIIO::ImageBuf input(spec);
    input.set_deep_samples(0,0,0,1);
    input.set_deep_value(0,0,0,0,0,0.5f);
    input.set_deep_value(0,0,0,1,0,1.0f);
    input.set_deep_value(0,0,0,2,0,2.0f);
    Job job;job.input=(root/"coverage.exr").u8string();job.options.visualization="auto";
    require(input.write(job.input),input.geterror().c_str());
    const auto coverage=job.input;
    OIIO::ImageSpec crypto_spec(1,1,4,OIIO::TypeDesc::FLOAT);
    crypto_spec.channelnames={"Object00.R","Object00.G","Object00.B","Object00.A"};
    crypto_spec.attribute("cryptomatte/test123/name","Object");
    crypto_spec.attribute("cryptomatte/test123/hash","MurmurHash3_32");
    crypto_spec.attribute("cryptomatte/test123/conversion","uint32_to_float32");
    OIIO::ImageBuf crypto(crypto_spec);
    const float ranks[]{0.1f,0.5f,0.0f,0.0f};crypto.setpixel(0,0,ranks);
    const auto matte=(root/"matte.exr").u8string();
    require(crypto.write(matte),crypto.geterror().c_str());
    for(const std::string type:{"encoded","result","cryptomatte"}) {
        job.id="changed-"+type;
        job.input=type=="cryptomatte"?matte:coverage;
        const auto path=root/(type+".png");
        const std::string previous="existing synthetic output";
        if(type=="result") {
            // Force a result without encoding or committing an output.
            std::ofstream output(path);output<<previous;
            require(bool(output),"cannot prepare an existing synthetic target");
        }
        job.outputs={{path.u8string(),"png",1,1,90}};
        Json result;
        run_isolated({job},worker,[&](const Json& value){result=value;});
        if(type!="result")require(!fs::exists(path),"a mismatched automatic preview published an output");
        else {
            std::ifstream output(path);std::string content;std::getline(output,content);
            require(content==previous,"the existing synthetic output changed");
        }
        require((result.at("status")=="failed" && result.at("error").at("code")=="worker_protocol_error" &&
            result.at("error").at("message").get<std::string>().find("worker automatic preview changed after admission")!=std::string::npos),
            result.dump().c_str());
        require(type=="cryptomatte"?result.at("cryptomatte").at("metadata_key")=="cryptomatte/test123":
            result.at("deep").at("mapping").at("zback")=="ZBack","the approved preview mapping changed");
    }
    for(const auto& entry:fs::directory_iterator(root))
        require(entry.path().filename().u8string().rfind(".thumbgen-tmp-",0)!=0,"mismatched automatic preview left a staging file");
}
#endif
static int test_main(const std::vector<std::string>& args){try {
#if defined(THUMBGEN_TEST_HOOKS)
    if(args.size()==5 && args[1]=="--internal-worker") {
        set_test_worker_message_hook(change_preview_mapping);
        return serve_worker(args[2],args[3],unsigned(std::stoi(args[4])));
    }
#endif
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
        // Deadline boundaries: the predicate outranks an expired deadline, a
        // finite deadline times out unchanged sequences, the infinite-deadline
        // path waits for the sequence, and pre-deadline changes still win.
        auto boundary=std::make_shared<ProcessWakeup>();
        auto base=boundary->sequence();boundary->notify();
        require(boundary->wait(base,Clock::now()),"pending change lost to an expired deadline");
        require(!boundary->wait(boundary->sequence(),Clock::now()),"unchanged sequence passed on an expired deadline");
        auto start=Clock::now();require(!boundary->wait(boundary->sequence(),start+30ms),"finite deadline woke early");
        require(Clock::now()-start>=20ms,"finite deadline underran");
        std::promise<bool> finished;auto done=finished.get_future();
        std::thread waiter([&]{finished.set_value(boundary->wait(boundary->sequence(),Clock::time_point::max()));});
        std::this_thread::sleep_for(50ms);
        require(done.wait_for(1ms)!=std::future_status::ready,"infinite deadline returned without a change");
        boundary->notify();require(done.get(),"infinite deadline lost the notification");waiter.join();
        base=boundary->sequence();std::thread notifier([&]{std::this_thread::sleep_for(20ms);boundary->notify();});
        require(boundary->wait(base,Clock::now()+3s),"notification before a finite deadline lost");notifier.join();
    }
#if defined(THUMBGEN_TEST_HOOKS)
    {
        // A condition-variable wake without a sequence change must not end the
        // wait: the predicated wait keeps blocking until a real notification.
        auto spurious_target=std::make_shared<ProcessWakeup>();auto base=spurious_target->sequence();
        std::promise<bool> finished;auto done=finished.get_future();
        std::thread waiter([&]{finished.set_value(spurious_target->wait(base,Clock::now()+3s));});
        std::this_thread::sleep_for(50ms);
        for(int i=0;i<5;++i)spurious_target->spurious();
        require(done.wait_for(100ms)!=std::future_status::ready,"spurious cv wake ended a predicated wait");
        spurious_target->notify();require(done.get(),"genuine notification lost after spurious wakes");waiter.join();
    }
#endif
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
#if defined(THUMBGEN_TEST_HOOKS)
    controller_scan_hook(args);
    const auto root=fs::temp_directory_path()/("thumbgen-preview-plan-"+std::to_string(Clock::now().time_since_epoch().count()));
    require(fs::create_directory(root),"cannot create synthetic preview test directory");
    struct Cleanup {fs::path root;~Cleanup(){std::error_code ec;fs::remove_all(root,ec);}} cleanup{root};
    check_automatic_preview_admission(fs::absolute(fs::u8path(args[0])).u8string(),root);
#endif
    std::cout<<"notification: pre-wait, scan races, deadline boundaries, empty wakes, concurrent children, EOF/fault, stop and lifetime passed";
#if defined(THUMBGEN_TEST_HOOKS)
    std::cout<<", spurious cv wakes and controller scan interleave passed";
#endif
    std::cout<<'\n';
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
