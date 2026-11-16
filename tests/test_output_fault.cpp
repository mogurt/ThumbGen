#include <thumbgen/core.h>
#include "output_fault.h"
#include "files.h"
#ifdef THUMBGEN_EXPECT_C2
#include "c2_resize.h"
#endif
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>

namespace fs=std::filesystem;
using thumbgen::Json;
using thumbgen::Job;
using thumbgen::Engine;
using thumbgen::detail::FileFailure;
static void need(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
static std::string bytes(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;need(bool(f),"write failed");}
struct Case {std::string name,phase="",kind="throw",policy="error",race="";int index=0,count=1,existing=-1;bool late_read=false;};
static const Case* active=nullptr;
static Json trace;
static bool hook(const char* phase,std::size_t index){
    trace.push_back({{"phase",phase},{"index",index}});
    if(!active)return false;
    const std::string key=phase;
    if(key=="read" && active->late_read && index>=1000)throw FileFailure("decode_failed","injected source read failure");
    if(key!=active->phase || (key=="read" || key=="resize_row"?index<std::size_t(active->index):index!=std::size_t(active->index)))return false;
#ifdef THUMBGEN_EXPECT_C2
    if(key=="invariant")throw thumbgen::detail::C2ContractError("injected workspace invariant failure");
#endif
    if(key=="sampler")return true;
    if(key=="preview"){
        if(active->kind=="null")return true;
        throw std::bad_alloc();
    }
    throw FileFailure(key=="read"?"decode_failed":key=="encode"?"encode_failed":key=="commit"?"commit_failed":"resize_failed","injected "+key+" failure");
}
static void fixture(const fs::path& path){
    OIIO::ImageSpec spec(4000,2000,4,OIIO::TypeDesc::UINT16);spec.alpha_channel=3;
    spec.attribute("oiio:UnassociatedAlpha",1);
    std::vector<std::uint16_t> pixels(std::size_t(spec.width)*spec.height*4);
    for(std::size_t i=0;i<pixels.size();i+=4){pixels[i]=std::uint16_t((i/4)%65536);pixels[i+1]=30000;pixels[i+2]=65535;pixels[i+3]=std::uint16_t(((i/4)%7)*10922);}
    auto out=OIIO::ImageOutput::create("png");need(bool(out),"writer");need(out->open(path.u8string(),spec),out->geterror());
    need(out->write_image(OIIO::TypeDesc::UINT16,pixels.data()),out->geterror());need(out->close(),out->geterror());
    // OIIO may generate eXIf. A pure PNG fixture keeps exact pixel data and
    // removes generated ancillary chunks, matching the declared stream scope.
    auto source=bytes(path);std::string clean=source.substr(0,8);
    for(std::size_t pos=8;pos+12<=source.size();){
        auto n=(std::uint32_t((unsigned char)source[pos])<<24)|(std::uint32_t((unsigned char)source[pos+1])<<16)|
            (std::uint32_t((unsigned char)source[pos+2])<<8)|std::uint32_t((unsigned char)source[pos+3]);
        need(n<=source.size()-pos-12,"PNG fixture length");auto type=source.substr(pos+4,4);
        if(type=="IHDR" || type=="IDAT" || type=="IEND")clean+=source.substr(pos,n+12);
        pos+=n+12;
    }
    write(path,clean);
}
static Json contract(const Json& r){
    Json v={{"ok",r.at("ok")},{"status",r.at("status")},{"outputs",Json::array()}};
    if(r.contains("error"))v["error"]=r["error"];
    if(r.contains("source_check"))v["source_stable"]=r["source_check"]["stable"];
    for(const auto& o:r["outputs"]){Json n=o;for(const char* k:{"path","resize_ms","finish_ms"})n.erase(k);v["outputs"].push_back(n);}
    return v;
}
int main(int argc,char** argv){
    fs::path root=argc>1?fs::u8path(argv[1]):fs::temp_directory_path()/("thumbgen-output-fault-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const bool keep=argc>1;
    try{
        need(fs::create_directory(root),"fresh test output required");auto input=root/"source.png";fixture(input);const auto original=bytes(input);
        std::vector<Case> cases={
            {"single-ok"},{"single-oom","preview"},{"single-null","preview","null"},
            {"single-resize","resize"},{"single-sampler-null","sampler"},
            {"single-sampler-error-exists","sampler","null","error","",0,1,0},
            {"sampler-late-read","sampler","null","error","",0,1,-1,true},{"single-read-first","read"},{"single-read-middle","read","throw","error","",1000},
            {"oom-late-read","preview","throw","error","",0,1,-1,true},
            {"single-encode","encode"},{"single-commit","commit"},
            {"single-error-exists","preview","throw","error","",0,1,0},
            {"single-skip-exists","preview","throw","skip","",0,1,0},
            {"single-replace-exists","preview","throw","replace","",0,1,0},
            {"single-error-appears","preview","throw","error","decoded-create"},
            {"single-skip-appears","preview","throw","skip","decoded-create"},
            {"single-error-disappears","preview","throw","error","decoded-remove",0,1,0},
            {"single-skip-race","","throw","skip","encoded-create"},
            {"single-error-race","","throw","error","encoded-create"},
            {"single-replace-race","","throw","replace","encoded-create"},
            {"single-source-decoded","preview","throw","error","decoded-source"},
            {"single-source-encoded","","throw","error","encoded-source"},
            {"dual-ok","","throw","error","",0,2},
            {"dual-first-oom","preview","throw","error","",0,2},
            {"dual-first-sampler-null","sampler","null","error","",0,2},
            {"dual-second-sampler-null","sampler","null","error","",1,2},
            {"dual-second-oom","preview","throw","error","",1,2},
            {"dual-first-null","preview","null","error","",0,2},
            {"dual-second-null","preview","null","error","",1,2},
            {"dual-first-resize","resize","throw","error","",0,2},
            {"dual-second-resize","resize","throw","error","",1,2},
            {"dual-first-encode","encode","throw","error","",0,2},
            {"dual-second-commit","commit","throw","error","",1,2},
            {"dual-skip-first","preview","throw","skip","",0,2,0},
            {"dual-skip-second-oom","preview","throw","skip","",1,2,0},
            {"dual-error-first","preview","throw","error","",0,2,0},
            {"dual-error-second","preview","throw","error","",1,2,1},
            {"dual-skip-disappears","","throw","skip","decoded-remove",0,2,0},
            {"dual-error-appears","preview","throw","error","decoded-create",0,2},
            {"dual-skip-race","","throw","skip","encoded-create",0,2},
            {"dual-source-encoded","","throw","error","encoded-source",0,2},
        };
#ifdef THUMBGEN_EXPECT_C2
        cases.push_back({"single-resize-midstream","resize_row","throw","error","",1000});
        cases.push_back({"resize-midstream-late-read","resize_row","throw","error","",500,1,-1,true});
        cases.push_back({"invariant-before-output-error","invariant","throw","error","",0,1,0});
        cases.push_back({"invariant-before-skip-race","invariant","throw","skip","decoded-create"});
#endif
        Json records=Json::array();std::size_t c2_allocations=0;bool planned=false;std::string race;
        Engine engine([&](const std::string& phase,const Job& job){
            if(phase=="c2_allocate"){need(planned,"C2 allocation before admission");++c2_allocations;}
            if(race==phase+"-create")write(fs::u8path(job.outputs[0].path),"competitor");
            if(race==phase+"-remove")fs::remove(fs::u8path(job.outputs[0].path));
            if(race==phase+"-source")write(input,original+"source changed");
        });
        Json plan;Engine::Control control;control.planned=[&](const Job&,const Json& value){planned=true;plan=value;};engine.set_control(control);
        thumbgen::detail::output_fault_hook=hook;
        for(const auto& c:cases){
            auto dir=root/c.name;fs::create_directory(dir);Job job;job.id=c.name;job.input=input.u8string();
            job.options.threads=1;job.options.background="transparent";job.options.max_decode_mib=2048;
            for(int k=0;k<c.count;++k)job.outputs.push_back({(dir/(std::to_string(k)+".png")).u8string(),"png",k?960:425,k?960:260,90,k==0?c.policy:"error"});
            if(c.existing>=0)write(fs::u8path(job.outputs[c.existing].path),"existing");
            active=&c;race=c.race;planned=false;c2_allocations=0;plan=nullptr;trace=Json::array();
            auto result=engine.convert(job);active=nullptr;race.clear();
            need(result.contains("ok"),result.dump());
            if(c.name=="single-ok" || c.name=="dual-ok")need(result["ok"]==true,result.dump());
            std::size_t last_row=0;bool saw_row=false;for(const auto& e:trace)if(e["phase"]=="read"){auto y=e["index"].get<std::size_t>();need(!saw_row || y>last_row,"source repeated or rewound");last_row=y;saw_row=true;}
            const bool stream=result.value("colour",Json::object()).value("processing",std::string{})=="integer-linear-stream-v1";
            if(c.count==2)need(!stream && c2_allocations==0 && !plan["footprint"].value("c2_stream",false),"dual must fall back before allocation");
#ifdef THUMBGEN_EXPECT_C2
            if(c.count==1 && c.name!="single-skip-exists")need(stream && c2_allocations==1,"single did not exercise C2");
#else
            need(!stream && c2_allocations==0,"reference unexpectedly streamed");
#endif
            if(c.name=="dual-first-oom" || c.name=="dual-first-null")need(result["status"]=="partial" && result["outputs"][1]["status"]=="success",result.dump());
            if(c.name=="dual-second-oom" || c.name=="dual-second-null")need(result["status"]=="partial" && result["outputs"][0]["status"]=="success",result.dump());
            if(c.late_read)need(result["error"]["code"]=="decode_failed" && result["error"]["stage"]=="decode" && result["outputs"].empty(),result.dump());
            if(c.phase=="invariant")need(result["error"]["code"]=="conversion_failed" && result["error"]["stage"]=="decode" && result["outputs"].empty(),"invariant error masked by output policy: "+result.dump());
            Json files=Json::array();for(const auto& o:job.outputs)files.push_back(fs::exists(fs::u8path(o.path))?Json(o.path):Json(nullptr));
            records.push_back({{"case",c.name},{"contract",contract(result)},{"files",files},{"stream",stream},{"c2_allocations",c2_allocations},{"trace",trace}});
            write(input,original);
            // The same resident Engine must recover after every injected failure.
            job.id=c.name+"-recovery";job.outputs={{(dir/"recovery.png").u8string(),"png",425,260,90}};planned=false;
            auto recovery=engine.convert(job);need(recovery["ok"]==true,recovery.dump());
            records.back()["recovery"]=contract(recovery);records.back()["recovery_file"]=job.outputs[0].path;
            for(const auto& e:fs::recursive_directory_iterator(dir))need(e.path().filename().u8string().rfind(".thumbgen-tmp-",0)!=0,"temporary leaked");
        }
        thumbgen::detail::output_fault_hook=nullptr;
        write(root/"results.json",records.dump(2));std::cout<<Json({{"ok",true},{"cases",records.size()}}).dump()<<'\n';
        if(!keep)fs::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<" evidence="<<root.u8string()<<'\n';return 1;}
}
