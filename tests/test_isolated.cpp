#include "isolated.h"
#include "child_process.h"
#include "worker_protocol.h"
#include "files.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace fs=std::filesystem;
using namespace thumbgen;
using namespace thumbgen::detail;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static void crash() {
#ifdef _WIN32
    RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
#else
    std::raise(SIGSEGV);
#endif
}
static void image(const fs::path& path,bool beauty=false,int canvas=0) {
    OIIO::ImageSpec spec(4,2,beauty?6:3,OIIO::TypeDesc::FLOAT);
    if(beauty)spec.channelnames={"R","G","B","Beauty.R","Beauty.G","Beauty.B"};
    if(canvas){spec.full_width=canvas;spec.full_height=canvas;}
    std::vector<float> pixels(size_t(8*spec.nchannels),beauty?0.0f:0.4f);
    if(beauty)for(size_t i=0;i<pixels.size();++i)if(i%6>=3)pixels[i]=0.8f;
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror());
    require(output->open(path.u8string(),spec),output->geterror());require(output->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),output->geterror());require(output->close(),output->geterror());
}
static std::string bytes(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
#ifndef _WIN32
static bool live_proc_stat(const std::string& stat) {
    auto end=stat.rfind(')');
    if(end==std::string::npos || end+2>=stat.size())return false;
    const auto state=stat[end+2];return state!='Z' && state!='X' && state!='x';
}
#endif
static bool running(unsigned long pid) {
#ifdef _WIN32
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));if(!process)return false;bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);return alive;
#else
    std::string stat;std::ifstream file("/proc/"+std::to_string(pid)+"/stat");std::getline(file,stat);return live_proc_stat(stat);
#endif
}
static int test_main(const std::vector<std::string>& args) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#else
    struct rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
    // /proc can transition from Z to X between the loop and final assertion.
    // Dead states cannot execute; all runnable, sleeping and stopped states
    // must still fail the worker ownership check.
    for(char state:std::string("RSDTtPIKW"))require(live_proc_stat(std::string("123 (worker ) helper) ")+state+" 1 123"),"live worker misclassified");
    for(char state:std::string("ZXx"))require(!live_proc_stat(std::string("123 (worker ) helper) ")+state+" 1 123"),"dead worker misclassified");
#endif
    if(args.size()==3 && args[1]=="--orphan-parent") {
        std::ifstream input(fs::u8path(args[2]));Json wire;input>>wire;
        run_isolated({job_from_wire(wire)},fs::absolute(fs::u8path(args[0])).u8string(),[](const Json&){});return 0;
    }
    if(args.size()==5 && args[1]=="--internal-worker") {
        std::map<std::string,int> encoded;
        return serve_worker(args[2],args[3],unsigned(std::stoi(args[4])),[&](const std::string& phase,const Job& job) {
#ifdef THUMBGEN_C2_TESTS
            if(phase=="decoded" && job.id=="c2-crash")crash();
            if(phase=="encoded" && job.id=="c2-partial" && ++encoded[job.id]==2)crash();
            if(phase=="decoded" && job.id=="c2-protocol")std::cout<<"not-json\n"<<std::flush;
            if(phase=="planned" && job.id=="c2-source-change"){std::ofstream changed(fs::u8path(job.input),std::ios::app|std::ios::binary);changed<<"source mutation";}
            if(phase=="decoded" && (job.id=="c2-sleep" || job.id=="c2-cancel")){
                {std::ofstream marker(fs::u8path(job.input+".ready"));marker<<"ready";}
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
#endif
            if(phase=="encoded" && (job.id=="partial" || job.id=="parallel-partial") && ++encoded[job.id]==2)crash();
            if(phase=="decoded" && (job.id=="crash" || job.id=="restart-anchor" || job.id=="budget" || job.id=="data-crash"))crash();
            if(phase=="committed" && job.id=="after-commit")crash();
            if(phase=="decoded" && job.id=="protocol")std::cout<<"not-json\n"<<std::flush;
            if(phase=="decoded" && job.id=="unknown-error")throw FileFailure("not_registered","injected unknown code");
            if(phase=="decoded" && job.id=="sleep")std::this_thread::sleep_for(std::chrono::seconds(10));
            if(phase=="decoded" && (job.id=="parallel-anchor" || job.id=="parallel-frame")) {
                {std::ofstream marker(fs::u8path(job.input+".ready"));marker<<"ready";}
                auto peer=fs::u8path(job.input).parent_path().parent_path()/(job.id=="parallel-anchor"?"parallel-frame":"parallel-anchor")/"source.exr.ready";
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!fs::exists(peer) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                require(fs::exists(peer),"independent workers did not overlap in decode");
            }
            if(phase=="decoded" && job.id=="budget-slow") {
                auto marker=fs::u8path(job.input).parent_path().parent_path()/"after-queue-crash"/"source.exr.planned";
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!fs::exists(marker) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                require(fs::exists(marker),"queued worker failure did not release its preparation lane");
            }
            if(phase=="committed" && job.id=="budget-slow"){std::ofstream marker(fs::u8path(job.input+".done"));marker<<"committed";}
            if(phase=="planned" && (job.id=="after-queue-crash" || job.id=="cancel-wait")){std::ofstream marker(fs::u8path(job.input+".planned"));marker<<"planned";}
            if(phase=="decoded" && job.id=="after-queue-crash") {
                auto marker=fs::u8path(job.input).parent_path().parent_path()/"budget-slow"/"source.exr.done";
                require(fs::exists(marker),"memory budget admitted a second decoder before the active task released resources");
            }
            if(phase=="planned" && job.id=="queued-crash")std::thread([]{std::this_thread::sleep_for(std::chrono::milliseconds(200));crash();}).detach();
            if(phase=="decoded" && job.id=="emit-first") {
                auto marker=fs::u8path(job.input).parent_path().parent_path()/"emit-hold"/"source.exr.pid";
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!fs::exists(marker) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                require(fs::exists(marker),"consumer failure did not overlap another encoder");
            }
            if(phase=="planned" && job.id=="source-change"){std::ofstream file(fs::u8path(job.input),std::ios::app|std::ios::binary);file<<"producer write";}
            if((phase=="decoded" && (job.id=="parent-loss" || job.id=="cancel-block")) || (phase=="encoded" && (job.id=="emit-hold" || job.id=="cancel-encoding"))) {
                {std::ofstream marker(fs::u8path(job.input+".pid"));marker<<
#ifdef _WIN32
                    GetCurrentProcessId()
#else
                    getpid()
#endif
                ;}
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        });
    }
    auto root=fs::temp_directory_path()/("thumbgen-isolated-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(root);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;for(const auto& dir:fs::directory_iterator(path,ec)){for(const auto& file:fs::directory_iterator(dir.path(),ec))fs::remove(file.path(),ec);fs::remove(dir.path(),ec);}fs::remove(path,ec);}} cleanup{root};
    try {
        auto executable=fs::absolute(fs::u8path(args[0])).u8string();Json all=Json::array();std::map<std::string,std::string> originals;
        auto job=[&](const std::string& id,int count=1,bool beauty=false) {
            auto dir=root/id;fs::create_directory(dir);Job value;value.id=id;value.input=(dir/"source.exr").u8string();value.options.input_space="linear";image(fs::u8path(value.input),beauty);originals[value.input]=bytes(fs::u8path(value.input));
            for(int i=0;i<count;++i)value.outputs.push_back({(dir/(std::to_string(i)+".png")).u8string(),"png",4,2,90});return value;
        };
        auto run=[&](std::vector<Job> jobs,IsolationOptions options=IsolationOptions{},std::string exe=std::string{}) {
            Json records=Json::array();bool interrupted=run_isolated(jobs,exe.empty()?executable:exe,[&](const Json& record){records.push_back(record);all.push_back(record);},options);
            if(options.cancelled)require(interrupted,"accepted cancellation was not returned to caller");
            require(records.size()==jobs.size(),"isolated batch lost a record");
            std::map<std::string,Json> by_id;for(const auto& record:records)require(by_id.emplace(record["id"].get<std::string>(),record).second,"duplicate isolated result");
            Json ordered=Json::array();for(const auto& item:jobs){require(by_id.count(item.id)!=0,"isolated id mismatch");ordered.push_back(by_id.at(item.id));}return ordered;
        };
        auto normal=job("normal"),partial=job("partial",2),after=job("after");auto rows=run({normal,partial,after});
        require(rows[0]["status"]=="success" && rows[1]["status"]=="partial" && rows[1]["outputs"][0]["status"]=="success" && rows[1]["outputs"][1]["error"]["code"]=="worker_crashed" && rows[2]["status"]=="success",rows.dump());
        require(fs::exists(fs::u8path(partial.outputs[0].path)) && !fs::exists(fs::u8path(partial.outputs[1].path)),"partial commit not preserved");
        auto anchor=job("restart-anchor");anchor.sequence_id="locked";auto next=job("restart-frame",1,true);next.sequence_id=anchor.sequence_id;
        rows=run({anchor,next});require(rows[0]["error"]["code"]=="worker_crashed" && rows[1]["status"]=="success" && rows[1]["selection"]["chosen"]["group"]=="RGB",rows.dump());
        OIIO::ImageBuf black(next.outputs[0].path);require(black.read(),black.geterror());float pixel[3];black.getpixel(0,0,pixel,3);require(pixel[0]==0 && pixel[1]==0 && pixel[2]==0,"worker restart changed primary image");
        auto data_crash=job("data-crash"),data_after=job("data-after");
        for(auto* value:{&data_crash,&data_after}) {
            value->sequence_id="data-lock";value->options.input_space="auto";value->options.visualization="depth";
            value->options.channels="R";value->options.data_range={0,1};
        }
        rows=run({data_crash,data_after});
        require(rows[0]["error"]["code"]=="worker_crashed" && rows[1]["status"]=="success" && rows[1]["selection"]["lock_status"]=="reused" &&
            rows[1]["visualization"]["range"]==Json::array({0,1}) && rows[1]["execution"]["worker_id"]!=rows[0]["execution"]["worker_id"],rows.dump());
        OIIO::ImageBuf data_preview(data_after.outputs[0].path);require(data_preview.read(),data_preview.geterror());data_preview.getpixel(0,0,pixel,3);
        require(pixel[0]>.399f && pixel[0]<.401f && pixel[0]==pixel[1] && pixel[1]==pixel[2],"data restart applied a colour transform");
        rows=run({job("protocol"),job("unknown-error"),job("after-protocol")});require(rows[0]["error"]["code"]=="worker_protocol_error" && rows[1]["error"]["code"]=="worker_protocol_error" && rows[2]["status"]=="success",rows.dump());
        rows=run({job("after-commit")});require(rows[0]["status"]=="success" && !rows[0]["warnings"].empty(),rows.dump());
        IsolationOptions budget;budget.memory_mib=129;rows=run({job("budget")},budget);require(rows[0]["error"]["code"]=="task_memory_exceeds_budget",rows.dump());
        IsolationOptions timeout;timeout.task_timeout=std::chrono::milliseconds(250);rows=run({job("sleep")},timeout);require(rows[0]["error"]["code"]=="worker_timeout",rows.dump());
        rows=run({job("missing-worker")},{},executable+".missing");require(rows[0]["error"]["code"]=="worker_start_failed",rows.dump());
        // Old glibc may report exec failure through either EOF or a pipe fault.
        // Both happen before the hello acknowledgement and are startup failures.
        for(int i=0;i<16;++i)run_isolated({normal},executable+".missing",[](const Json& record) {
            require(record["status"]=="failed" && record["error"]["code"]=="worker_start_failed",record.dump());
        });
        auto changing=job("source-change");rows=run({changing});require(rows[0]["error"]["code"]=="source_changed" && rows[0]["source_check"]["stable"]==false,rows.dump());
        {std::ofstream restore(fs::u8path(changing.input),std::ios::binary);restore<<originals.at(changing.input);}
        auto missing=job("missing-anchor");missing.sequence_id="unavailable";missing.input=(root/"missing.exr").u8string();auto dependent=job("dependent");dependent.sequence_id=missing.sequence_id;
        auto conflicting=job("before-anchor-conflict");conflicting.sequence_id=missing.sequence_id;conflicting.options.selection_policy="root_rgb";
        auto later=job("after-anchor",1,true);later.sequence_id=missing.sequence_id;
        rows=run({missing,conflicting,dependent,later});require(rows[0]["error"]["code"]=="open_failed" && rows[1]["error"]["code"]=="sequence_selection_conflict" && rows[2]["status"]=="success" && rows[3]["status"]=="success" && rows[3]["selection"]["chosen"]["group"]=="RGB",rows.dump());
        auto skip=job("skip-anchor");skip.sequence_id="skip-lock";skip.outputs[0].existing="skip";{std::ofstream output(fs::u8path(skip.outputs[0].path));output<<"existing";}
        auto frame=job("skip-next",1,true);frame.sequence_id=skip.sequence_id;rows=run({skip,frame});require(rows[0]["status"]=="skipped" && rows[1]["status"]=="success" && rows[1]["selection"]["chosen"]["group"]=="RGB",rows.dump());
        IsolationOptions pool;pool.workers=2;pool.thread_budget=2;pool.memory_mib=1024;
        auto parallel_anchor=job("parallel-anchor"),parallel_frame=job("parallel-frame",1,true);
        parallel_anchor.sequence_id="parallel-lock";parallel_frame.sequence_id=parallel_anchor.sequence_id;
        rows=run({parallel_anchor,parallel_frame},pool);
        require(rows[0]["status"]=="success" && rows[1]["status"]=="success" && rows[1]["selection"]["chosen"]["group"]=="RGB",rows.dump());
        require(rows[0]["execution"]["worker_id"]!=rows[1]["execution"]["worker_id"] && rows[0]["execution"]["threads"]==1,"parallel quotas/ownership lost");
        for(unsigned workers:{1u,2u,4u,8u}) {
            pool.workers=workers;pool.thread_budget=workers*2;pool.memory_mib=8192;
            std::vector<Job> inputs;for(unsigned i=0;i<workers*2;++i) {
                auto value=job("gradient-"+std::to_string(workers)+"-"+std::to_string(i));if(i%2==0)value.options.threads=1;inputs.push_back(std::move(value));
            }
            rows=run(inputs,pool);for(size_t i=0;i<rows.size();++i)require(rows[i]["status"]=="success" && rows[i]["execution"]["threads"]==(i%2==0?1:2) && rows[i]["execution"]["workers"]==workers,rows[i].dump());
        }
        pool.workers=2;pool.thread_budget=2;pool.memory_mib=402;
        rows=run({job("budget-slow"),job("queued-crash"),job("after-queue-crash")},pool);
        require(rows[0]["status"]=="success" && rows[1]["error"]["code"]=="worker_crashed" && rows[2]["status"]=="success",rows.dump());
        require(rows[2]["execution"]["queue_ms"].get<double>()>0,"low memory waiting diagnostic missing");
        pool.memory_mib=1024;
        rows=run({job("crash"),job("concurrent-normal"),job("after-concurrent-crash")},pool);
        require(rows[0]["error"]["code"]=="worker_crashed" && rows[1]["status"]=="success" && rows[2]["status"]=="success",rows.dump());
        rows=run({job("parallel-partial",2),job("concurrent-with-partial")},pool);
        require(rows[0]["status"]=="partial" && rows[0]["outputs"][0]["status"]=="success" && rows[0]["outputs"][1]["status"]=="failed" && rows[1]["status"]=="success",rows.dump());
        auto oversized=job("oversized-head");image(fs::u8path(oversized.input),false,2048);originals[oversized.input]=bytes(fs::u8path(oversized.input));
        oversized.outputs[0].width=oversized.outputs[0].height=1024;pool.memory_mib=402;
        rows=run({oversized,job("after-oversized-head")},pool);
        require(rows[0]["error"]["code"]=="task_memory_exceeds_budget" && rows[1]["status"]=="success",rows.dump());
        auto pool_missing=job("pool-missing");pool_missing.input=(root/"missing-pool.exr").u8string();pool_missing.sequence_id="pool-missing-lock";
        auto pool_conflict=job("pool-conflict");pool_conflict.sequence_id=pool_missing.sequence_id;pool_conflict.options.selection_policy="root_rgb";
        auto pool_valid=job("pool-valid");pool_valid.sequence_id=pool_missing.sequence_id;
        auto pool_later=job("pool-later",1,true);pool_later.sequence_id=pool_missing.sequence_id;pool.memory_mib=1024;
        rows=run({pool_missing,pool_conflict,pool_valid,pool_later},pool);
        require(rows[0]["error"]["code"]=="open_failed" && rows[1]["error"]["code"]=="sequence_selection_conflict" && rows[2]["status"]=="success" && rows[3]["status"]=="success" && rows[3]["selection"]["chosen"]["group"]=="RGB",rows.dump());
        auto emit_first=job("emit-first"),emit_hold=job("emit-hold");bool consumer_failed=false;
        try {run_isolated({emit_first,emit_hold},executable,[](const Json&){throw std::runtime_error("consumer stopped");},pool);}
        catch(const std::runtime_error& e){consumer_failed=std::string(e.what())=="consumer stopped";}
        unsigned long held_pid=0;{std::ifstream marker(fs::u8path(emit_hold.input+".pid"));marker>>held_pid;}
        require(consumer_failed && held_pid && !running(held_pid),"consumer failure leaked an encoding worker");
        IsolationOptions cancel;cancel.cancelled=[] {return true;};
        rows=run({job("cancel-before-a"),job("cancel-before-b"),job("cancel-before-c")},cancel);
        for(const auto& row:rows)require(row["status"]=="cancelled" && !row["execution"].contains("worker_id"),row.dump());
        auto cancel_partial=job("cancel-partial",2);cancel.cancelled=[&] {return fs::exists(fs::u8path(cancel_partial.outputs[0].path));};
        rows=run({cancel_partial,job("cancel-after-partial")},cancel);
        require(rows[0]["status"]=="cancelled" && rows[0]["outputs"][0]["status"]=="success" && rows[0]["outputs"][1]["status"]=="cancelled" && rows[1]["status"]=="cancelled",rows.dump());
        auto cancel_complete=job("cancel-after-commit");cancel.cancelled=[&] {return fs::exists(fs::u8path(cancel_complete.outputs[0].path));};
        rows=run({cancel_complete},cancel);require(rows[0]["status"]=="success",rows.dump());
        auto cancel_block=job("cancel-block"),cancel_wait=job("cancel-wait");cancel=pool;cancel.memory_mib=402;
        cancel.cancelled=[&] {return fs::exists(fs::u8path(cancel_block.input+".pid")) && fs::exists(fs::u8path(cancel_wait.input+".planned"));};
        rows=run({cancel_block,cancel_wait,job("cancel-unstarted")},cancel);
        for(const auto& row:rows)require(row["status"]=="cancelled",row.dump());
        unsigned long cancelled_pid=0;{std::ifstream marker(fs::u8path(cancel_block.input+".pid"));marker>>cancelled_pid;}
        require(cancelled_pid && !running(cancelled_pid),"cancelled decoder survived");
        auto cancel_encoding=job("cancel-encoding");cancel_encoding.outputs[0].existing="replace";
        {std::ofstream existing(fs::u8path(cancel_encoding.outputs[0].path));existing<<"preserve previous target";}
        cancel=IsolationOptions{};cancel.cancelled=[&] {return fs::exists(fs::u8path(cancel_encoding.input+".pid"));};
        rows=run({cancel_encoding},cancel);require(rows[0]["status"]=="cancelled" && bytes(fs::u8path(cancel_encoding.outputs[0].path))=="preserve previous target",rows.dump());
        auto orphan=job("parent-loss");auto wire=fs::u8path(orphan.input+".json");{std::ofstream output(wire);output<<job_to_wire(orphan);}
        {
            ChildProcess owner(executable,{"--orphan-parent",wire.u8string()});auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);unsigned long pid=0;
            while(!pid && std::chrono::steady_clock::now()<deadline){std::ifstream marker(fs::u8path(orphan.input+".pid"));marker>>pid;if(!pid)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            require(pid && running(pid),"parent-loss worker did not reach decode");owner.stop();deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(running(pid) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(!running(pid),"worker survived forced owner exit");
        }
#ifdef THUMBGEN_C2_TESTS
        // Exercise the real opt-in path in the same controller/worker fault
        // harness, rather than transferring tiny EXR fault coverage to C2.
        fs::create_directory(root/"c2-template");auto png=root/"c2-template"/"source.png";
        {
            OIIO::ImageSpec spec(4000,2000,4,OIIO::TypeDesc::UINT16);spec.attribute("oiio:UnassociatedAlpha",1);
            auto writer=OIIO::ImageOutput::create(png.u8string());require(bool(writer),OIIO::geterror());require(writer->open(png.u8string(),spec),writer->geterror());
            std::vector<std::uint16_t> row(4000*4,32768);
            for(int y=0;y<2000;++y)require(writer->write_scanline(y,0,OIIO::TypeDesc::UINT16,row.data()),writer->geterror());
            require(writer->close(),writer->geterror());
        }
        {
            // The PNG writer may add ancillary EXIF/CICP. This synthetic
            // pixel fixture deliberately contains only IHDR/IDAT/IEND; tests
            // of unknown metadata fallback are separate and keep their data.
            const auto encoded=bytes(png);std::string plain=encoded.substr(0,8);
            for(size_t offset=8;offset<encoded.size();){
                require(encoded.size()-offset>=12,"short fixture chunk");
                const auto* h=reinterpret_cast<const unsigned char*>(encoded.data()+offset);
                const auto length=(std::uint32_t(h[0])<<24)|(std::uint32_t(h[1])<<16)|(std::uint32_t(h[2])<<8)|h[3];
                require(length<=encoded.size()-offset-12,"fixture chunk overrun");
                const auto type=encoded.substr(offset+4,4);
                if(type=="IHDR" || type=="IDAT" || type=="IEND")plain.append(encoded,offset,size_t(length)+12);
                else std::cerr<<"C2 synthetic fixture excludes generated "<<type<<" metadata\n";
                offset+=size_t(length)+12;
            }
            std::ofstream file(png,std::ios::binary);file.write(plain.data(),plain.size());require(bool(file),"fixture write failed");
        }
        auto c2job=[&](const std::string& id,int count=1){
            auto dir=root/id;fs::create_directory(dir);Job value;value.id=id;value.input=(dir/"source.png").u8string();
            fs::copy_file(png,fs::u8path(value.input));originals[value.input]=bytes(fs::u8path(value.input));
            value.options.threads=1;value.options.background="transparent";
            for(int i=0;i<count;++i)value.outputs.push_back({(dir/(std::to_string(i)+".png")).u8string(),"png",i?960:425,i?960:260,90});return value;
        };
        auto c2normal=c2job("c2-normal",2);rows=run({c2normal,c2job("c2-crash"),c2job("c2-after-crash")});
        require(rows[0]["ok"]==true && rows[0]["colour"]["processing"]=="integer-linear-stream-v1" && rows[1]["error"]["code"]=="worker_crashed" && rows[2]["ok"]==true,rows.dump());
        rows=run({c2job("c2-partial",2),c2job("c2-protocol"),c2job("c2-after-protocol")});
        require(rows[0]["status"]=="partial" && rows[0]["outputs"][0]["status"]=="success" && rows[0]["outputs"][1]["error"]["code"]=="worker_crashed" && rows[1]["error"]["code"]=="worker_protocol_error" && rows[2]["ok"]==true,rows.dump());
        auto c2changed=c2job("c2-source-change");rows=run({c2changed});require(rows[0]["error"]["code"]=="source_changed" && rows[0]["source_check"]["stable"]==false,rows.dump());
        {std::ofstream restore(fs::u8path(c2changed.input),std::ios::binary);restore<<originals.at(c2changed.input);}
        IsolationOptions c2timeout;c2timeout.task_timeout=std::chrono::milliseconds(500);rows=run({c2job("c2-sleep")},c2timeout);require(rows[0]["error"]["code"]=="worker_timeout",rows.dump());
        IsolationOptions c2budget;c2budget.memory_mib=129;rows=run({c2job("c2-low-memory")},c2budget);require(rows[0]["error"]["code"]=="task_memory_exceeds_budget",rows.dump());
        auto c2cancel=c2job("c2-cancel");IsolationOptions c2stop;c2stop.cancelled=[&]{return fs::exists(fs::u8path(c2cancel.input+".ready"));};
        rows=run({c2cancel},c2stop);require(rows[0]["status"]=="cancelled" && rows[0]["error"]["code"]=="task_cancelled",rows.dump());
        rows=run({c2job("c2-after-cancel")});require(rows[0]["ok"]==true,rows.dump());
#endif
        for(const auto& original:originals)require(bytes(fs::u8path(original.first))==original.second,"isolated source changed");
        for(const auto& entry:fs::recursive_directory_iterator(root))require(entry.path().filename().u8string().rfind(".thumbgen-tmp-",0)!=0,"isolated temporary leaked");
        std::cout<<Json({{"ok",true},{"records",all}}).dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;for(int i=0;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),n,nullptr,nullptr);value.pop_back();args.push_back(std::move(value));}return test_main(args);
}
#else
int main(int argc,char** argv){return test_main(std::vector<std::string>(argv,argv+argc));}
#endif
