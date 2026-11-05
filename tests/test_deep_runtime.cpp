#include "isolated.h"
#include "files.h"
#include <OpenImageIO/imagebuf.h>
#include <chrono>
#include <csignal>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
static void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
static void crash() {
#ifdef _WIN32
    RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
#else
    std::raise(SIGSEGV);
#endif
}
static unsigned long pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
static bool running(unsigned long value) {
#ifdef _WIN32
    auto handle=OpenProcess(SYNCHRONIZE,FALSE,DWORD(value));if(!handle)return false;
    bool alive=WaitForSingleObject(handle,0)==WAIT_TIMEOUT;CloseHandle(handle);return alive;
#else
    std::ifstream file("/proc/"+std::to_string(value)+"/stat");std::string stat;std::getline(file,stat);auto end=stat.rfind(')');
    return end!=std::string::npos && end+2<stat.size() && stat[end+2]!='Z';
#endif
}
static std::string bytes(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
static void fixture(const fs::path& path,bool beauty=false) {
    OIIO::ImageSpec spec(3,2,beauty?9:6,OIIO::TypeDesc::FLOAT);spec.deep=true;
    spec.channelnames={"R","G","B","A","Z","Zback"};
    if(beauty){spec.channelnames.push_back("beauty.R");spec.channelnames.push_back("beauty.G");spec.channelnames.push_back("beauty.B");}
    spec.alpha_channel=3;spec.z_channel=4;spec.attribute("compression","zips");spec.attribute("oiio:ColorSpace","linear");
    OIIO::ImageBuf image(spec);
    for(int y=0;y<2;++y)for(int x=0;x<3;++x) {
        image.set_deep_samples(x,y,0,2);
        for(int s=0;s<2;++s)for(int c=0;c<spec.nchannels;++c) {
            float value=0;
            if(c==0 || c==6)value=s==1?.5f:0;
            if(c==2 || c==8)value=s==0?.5f:0;
            if(beauty && c<3)value=0;
            if(c==3)value=.5f;if(c==4 || c==5)value=s==0?2.f:1.f;
            image.set_deep_value(x,y,0,c,s,value);
        }
    }
    require(image.write(path.u8string()),image.geterror());
}
static int test_main(const std::vector<std::string>& args) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#else
    struct rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
#endif
    if(args.size()==5 && args[1]=="--internal-worker") {
        std::map<std::string,int> encoded;
        return serve_worker(args[2],args[3],unsigned(std::stoi(args[4])),[&](const std::string& phase,const Job& job) {
            if(phase=="deep_counts" && job.id=="deep-crash-anchor")crash();
            if(phase=="encoded" && job.id=="deep-partial" && ++encoded[job.id]==2)crash();
            if(phase=="deep_counts" && job.id=="deep-source-change"){std::ofstream file(fs::u8path(job.input),std::ios::app|std::ios::binary);file<<"producer mutation";}
            const bool hold=(phase=="deep_counts" && (job.id=="deep-timeout" || job.id=="deep-cancel")) ||
                (phase=="committed" && job.id=="deep-cancel-committed");
            if(hold) {
                {std::ofstream marker(fs::u8path(job.input+".pending"));marker<<pid();}
                fs::rename(fs::u8path(job.input+".pending"),fs::u8path(job.input+".held"));
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        });
    }
    auto root=fs::temp_directory_path()/("thumbgen-deep-runtime-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(root),"cannot reserve Deep runtime directory");
    struct Cleanup{fs::path path;~Cleanup(){std::error_code error;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,error);}}cleanup{root};
    try {
        auto executable=fs::absolute(fs::u8path(args[0])).u8string();Json all=Json::array();std::map<std::string,std::string> originals;
        auto job=[&](const std::string& id,int outputs=1,bool beauty=false) {
            auto dir=root/id;require(fs::create_directory(dir),"job directory exists");Job value;value.id=id;value.input=(dir/"input.exr").u8string();
            value.options.deep_mode="flatten";value.options.max_decode_mib=1;value.options.threads=1;value.options.input_space="linear";value.options.ocio_config="ocio://default";
            fixture(fs::u8path(value.input),beauty);originals[value.input]=bytes(fs::u8path(value.input));
            for(int i=0;i<outputs;++i)value.outputs.push_back({(dir/(std::to_string(i)+".png")).u8string(),"png",3,2,90});
            return value;
        };
        auto run=[&](const std::vector<Job>& jobs,IsolationOptions options=IsolationOptions{}) {
            Json rows=Json::array();bool interrupted=run_isolated(jobs,executable,[&](const Json& record){rows.push_back(record);all.push_back(record);},options);
            require(interrupted==bool(options.cancelled),"Deep cancellation return mismatch");
            require(rows.size()==jobs.size(),"Deep fault lost results");
            for(std::size_t i=0;i<jobs.size();++i)require(rows[i]["id"]==jobs[i].id,"Deep fault changed FIFO result identity");
            return rows;
        };
        auto anchor=job("deep-crash-anchor"),frame=job("deep-restart-frame",1,true);anchor.sequence_id=frame.sequence_id="deep-shot";
        auto rows=run({anchor,frame});
        require(rows[0]["error"]["code"]=="worker_crashed" && rows[1]["status"]=="success" && rows[1]["selection"]["lock_status"]=="reused" &&
            rows[1]["selection"]["chosen"]["group"]=="RGB" && rows[0]["execution"]["worker_id"]!=rows[1]["execution"]["worker_id"],rows.dump());
        OIIO::ImageBuf black(frame.outputs[0].path);require(black.read(),black.geterror());float pixel[3];black.getpixel(0,0,pixel,3);
        require(pixel[0]==0 && pixel[1]==0 && pixel[2]==0,"Deep worker restart switched to newly present beauty");
        auto partial=job("deep-partial",2);rows=run({partial,job("deep-after-partial")});
        require(rows[0]["status"]=="partial" && rows[0]["outputs"][0]["status"]=="success" && rows[0]["outputs"][1]["error"]["code"]=="worker_crashed" && rows[1]["status"]=="success",rows.dump());
        require(fs::exists(fs::u8path(partial.outputs[0].path)) && !fs::exists(fs::u8path(partial.outputs[1].path)),"Deep partial ledger lost committed output");
        auto changed=job("deep-source-change");rows=run({changed,job("deep-after-change")});
        require(rows[0]["error"]["code"]=="source_changed" && rows[0]["source_check"]["stable"]==false && rows[1]["status"]=="success",rows.dump());
        require(bytes(fs::u8path(changed.input))==originals.at(changed.input)+"producer mutation","source mutation injection was not exact");originals.erase(changed.input);
        auto timeout=job("deep-timeout");IsolationOptions timed;timed.task_timeout=std::chrono::milliseconds(1500);rows=run({timeout,job("deep-after-timeout")},timed);
        require(rows[0]["error"]["code"]=="worker_timeout" && rows[1]["status"]=="success",rows.dump());
        auto stopped=[&](const Job& value){unsigned long held=0;{std::ifstream file(fs::u8path(value.input+".held"));file>>held;}require(held && !running(held),"Deep stopped worker survived or never reached the checkpoint");};
        stopped(timeout);
        auto first=job("deep-before-cancel"),cancelled=job("deep-cancel"),queued=job("deep-unstarted");IsolationOptions cancel;
        cancel.cancelled=[&]{return fs::exists(fs::u8path(cancelled.input+".held"));};rows=run({first,cancelled,queued},cancel);
        require(rows[0]["status"]=="success" && rows[1]["status"]=="cancelled" && rows[2]["status"]=="cancelled",rows.dump());stopped(cancelled);
        require(fs::exists(fs::u8path(first.outputs[0].path)) && !fs::exists(fs::u8path(cancelled.outputs[0].path)) && !fs::exists(fs::u8path(queued.outputs[0].path)),"Deep cancellation damaged output coverage");
        auto committed=job("deep-cancel-committed",2);cancel.cancelled=[&]{return fs::exists(fs::u8path(committed.input+".held"));};rows=run({committed,job("deep-after-commit-cancel")},cancel);
        require(rows[0]["status"]=="cancelled" && rows[0]["outputs"][0]["status"]=="success" && rows[0]["outputs"][1]["status"]=="cancelled" && rows[1]["status"]=="cancelled",rows.dump());stopped(committed);
        require(fs::exists(fs::u8path(committed.outputs[0].path)) && !fs::exists(fs::u8path(committed.outputs[1].path)),"Deep cancellation did not retain first commit");
        int readbacks=0;
        for(const auto& record:all)for(const auto& output:record["outputs"])if(output["status"]=="success") {
            OIIO::ImageBuf image(output["path"].get<std::string>());require(image.read(),image.geterror());
            require(image.spec().width==3 && image.spec().height==2 && image.nchannels()==3,"Deep committed output has invalid geometry");
            const bool locked_black=record["id"]=="deep-restart-frame";
            for(int y=0;y<2;++y)for(int x=0;x<3;++x) {
                float rgb[3];image.getpixel(x,y,rgb,3);
                const float expected[3]={locked_black?0.f:188.f/255,0.f,locked_black?0.f:137.f/255};
                for(int c=0;c<3;++c)require(std::isfinite(rgb[c]) && std::abs(rgb[c]-expected[c])<=1.01f/255,"Deep committed output pixel was corrupted by fault recovery");
            }
            ++readbacks;
        }
        require(readbacks==7,"Deep successful output readback count differs");
        for(const auto& original:originals)require(bytes(fs::u8path(original.first))==original.second,"Deep test changed an uninjected source");
        for(const auto& entry:fs::recursive_directory_iterator(root))require(entry.path().filename().u8string().rfind(".thumbgen-tmp-",0)!=0,"Deep fault leaked a temporary");
        std::cout<<Json({{"ok",true},{"records",all}}).dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;for(int i=0;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(std::size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),n,nullptr,nullptr);value.pop_back();args.push_back(std::move(value));}return test_main(args);
}
#else
int main(int argc,char** argv){return test_main(std::vector<std::string>(argv,argv+argc));}
#endif
