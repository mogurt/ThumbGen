// Synthetic inputs only. Run the same executable in baseline/candidate builds,
// separate roots, then compare the PNGs exactly (and success/failure records).
#include <thumbgen/core.h>
#include "../src/selected_input.h"
#include "../src/exr_profile.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <OpenImageIO/imageio.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
static void require(bool v,const std::string& s){if(!v)throw std::runtime_error(s);}
static void fixture(const fs::path& path,const std::vector<std::string>& names,bool tiled,bool half,int width=73,int height=41) {
    require(width>0 && height>0,"fixture dimensions");
    OIIO::ImageSpec s(width,height,int(names.size()),half?OIIO::TypeDesc::HALF:OIIO::TypeDesc::FLOAT);
    s.channelnames=names;s.alpha_channel=-1;
    for(int c=0;c<s.nchannels;++c)if(names[c]=="A")s.alpha_channel=c;
    s.attribute("compression","none");s.attribute("oiio:ColorSpace","linear");
    // Test-only reproducible metadata; the original eight shapes stay 73x41.
    s.attribute("DateTime","2000:01:01 00:00:00");
    if(tiled){s.tile_width=s.tile_height=16;s.tile_depth=1;}
    std::vector<float> p(std::size_t(s.width)*s.height*s.nchannels);
    for(std::size_t i=0;i<p.size();++i) {
        int c=int(i%names.size());auto pixel=i/names.size();
        p[i]=names[c]=="A"?(pixel%4==0?0.0f:pixel%4==1?0.001f:pixel%4==2?0.5f:1.0f):float((pixel*17+c*11)%101)/50.0f;
    }
    auto out=OIIO::ImageOutput::create(path.u8string());require(bool(out),"fixture writer");
    require(out->open(path.u8string(),s),out->geterror());
    require(out->write_image(OIIO::TypeDesc::FLOAT,p.data()),out->geterror());require(out->close(),out->geterror());
}
// Independent direct-read guard: both destinations are initialized, never
// invoke undefined behavior just to test an uninitialized-allocation candidate.
static bool sentinel_guard(const fs::path& path,bool sparse,std::ofstream& records) {
    std::vector<float> results[2];bool success[2]{},opened[2]{};std::size_t runs=0;
    for(int pass=0;pass<2;++pass) {
        auto in=OIIO::ImageInput::open(path.u8string());opened[pass]=bool(in);
        if(!in)continue; // Open-time rejection of damaged chunk offsets is safe.
        auto s=in->spec();std::vector<int> chosen;
        for(const char* name: {"R","G","B","A"}) {
            auto it=std::find(s.channelnames.begin(),s.channelnames.end(),name);
            if(it!=s.channelnames.end())chosen.push_back(int(it-s.channelnames.begin()));
        }
        if(chosen.empty())chosen={0};
        auto plan=thumbgen::detail::channel_plan(s,chosen,false);
        if(sparse) {
            // OIIO reorders EXR root channels into canonical RGBA before the
            // application sees them. Force a genuine gap in presented indices,
            // instead of assuming raw file alphabetical ordering survives.
            require(s.nchannels>=4,"sparse guard needs at least four channels");
            plan=thumbgen::detail::channel_plan(s,{0,2,3},false);
            require(plan.runs.size()>1,"fixture did not exercise disjoint runs");
        }
        runs=plan.runs.size();
        results[pass].assign(std::size_t(s.width)*s.height*plan.channels.size(),pass==0?12345.25f:-23456.5f);
        success[pass]=thumbgen::detail::read_selected(*in,s,0,plan,false,results[pass].data());
        in->close();
    }
    require(opened[0]==opened[1],"open status differs between guard passes");
    require(success[0]==success[1],"read success depends on destination prefill");
    if(success[0]) {
        require(results[0].size()==results[1].size(),"guard destination size");
        require(std::memcmp(results[0].data(),results[1].data(),results[0].size()*sizeof(float))==0,
                "successful read left destination-dependent samples");
        for(std::size_t i=0;i<results[0].size();++i)
            require(results[0][i]!=12345.25f && results[1][i]!=-23456.5f,"successful read retained sentinel");
    }
    records<<thumbgen::Json({{"guard",path.filename().u8string()},{"sparse",sparse},{"runs",runs},
        {"opened",opened[0]},{"ok",success[0]},{"samples",results[0].size()},{"success_prefill_independent",success[0]}}).dump()<<'\n';
    return success[0];
}
// Bounded metadata parse for our single-part scanline NONE fixtures. Derive
// truncation from actual offset table + first chunk leader, not a magic size.
static std::uint64_t first_chunk_midpoint(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);require(bool(in),"chunk inspection open");
    auto u32=[&]() {unsigned char b[4];in.read(reinterpret_cast<char*>(b),4);require(bool(in),"metadata truncated");
        return std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);};
    auto z=[&]() {std::string s;for(int i=0;i<256;++i){char c=0;in.get(c);require(bool(in),"identifier truncated");if(!c)return s;s+=c;}throw std::runtime_error("identifier bound");};
    require(u32()==20000630,"EXR magic");require(u32()==2,"flat scanline fixture version");
    for(int i=0;i<256;++i) {
        auto name=z();if(name.empty())break;auto type=z();(void)type;auto n=u32();require(n<1024*1024,"attribute bound");
        in.seekg(n,std::ios::cur);require(bool(in),"attribute seek");if(i==255)require(false,"attribute count bound");
    }
    auto lo=u32(),hi=u32();auto offset=std::uint64_t(lo)|(std::uint64_t(hi)<<32);
    require(offset<fs::file_size(path),"first chunk offset bounds");in.seekg(static_cast<std::streamoff>(offset));
    auto y=u32();auto bytes=u32();(void)y;
    require(bytes>1 && offset+8+bytes<=fs::file_size(path),"first chunk payload bounds");
    return offset+8+bytes/2;
}
static void check_nozero_marker(const thumbgen::Json& r,bool large_eligible) {
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
    const auto& times=r.at("timings_ms");
    require(times.contains("exr_nozero_hit") && times["exr_nozero_hit"].is_number(),"profile must expose numeric NOZERO route marker");
#if defined(THUMBGEN_EXR_NOZERO) && THUMBGEN_EXR_NOZERO
    const double expected=large_eligible?1.0:0.0;
#else
    const double expected=0.0;(void)large_eligible;
#endif
    require(times["exr_nozero_hit"].get<double>()==expected,"NOZERO large hit / small fallback marker mismatch");
#else
    (void)large_eligible;
    require(!r.at("timings_ms").contains("exr_nozero_hit"),"PROFILE OFF must not expose NOZERO route marker");
#endif
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: test_exr_pipeline EMPTY_OUTPUT_ROOT");fs::path root=fs::absolute(argv[1]);
        require(!fs::exists(root),"test output root must not exist");fs::create_directories(root);
        const std::vector<std::vector<std::string>> layouts={{"R","G","B"},{"R","G","B","A"},{"Y"},{"A","B","F.aux","G","R"}};
        thumbgen::Engine engine;std::ofstream records(root/"records.jsonl");
        for(std::size_t i=0;i<layouts.size();++i)for(int tiled=0;tiled<2;++tiled) {
            auto id=std::to_string(i)+"-"+std::to_string(tiled);auto source=root/(id+".exr");fixture(source,layouts[i],bool(tiled),i%2==0);
            require(sentinel_guard(source,false,records),"complete fixture read guard failed");
            thumbgen::Job job;job.id=id;job.input=source.u8string();job.options.input_space="linear";job.options.background="transparent";job.options.threads=1;
            for(int size:{19,37})job.outputs.push_back({(root/(id+"-"+std::to_string(size)+".png")).u8string(),"png",size,size,90});
            auto r=engine.convert(job);require(r["ok"].get<bool>(),r.dump());
            check_nozero_marker(r,false);
            for(auto& x:r["timings_ms"].items())require(x.value().is_number()&&x.value().get<double>()>=0,"numeric timings contract");
            // Do not write absolute paths in the portable comparison records.
            records<<thumbgen::Json({{"id",id},{"ok",r["ok"]},{"timings_ms",r["timings_ms"]}}).dump()<<'\n';
        }
        auto complete=root/"complete-guard.exr";fixture(complete,layouts[1],false,false);
        require(sentinel_guard(complete,false,records),"complete RGBA guard read failed");
        require(sentinel_guard(root/"3-0.exr",true,records),"noncontiguous scanline guard read failed");
        require(sentinel_guard(root/"3-1.exr",true,records),"noncontiguous tiled guard read failed");
        auto tail=root/"tail64.exr";fs::copy_file(complete,tail);auto n=fs::file_size(tail);require(n>128,"fixture length");fs::resize_file(tail,n-64);
        // OpenEXR NONE deliberately zero-fills a short packed payload. Preserve
        // the observed baseline tolerance, but prove all destination samples
        // are independent of their initialized prefill on success.
        const bool tail_ok=sentinel_guard(tail,false,records);
        thumbgen::Job tolerant;tolerant.input=tail.u8string();tolerant.options.input_space="linear";tolerant.options.background="transparent";
        tolerant.outputs={{(root/"tail64.png").u8string(),"png",19,19,90}};
        const auto tail_result=engine.convert(tolerant);
        require(tail_result["ok"].get<bool>()==tail_ok,"Engine/direct tail64 outcome mismatch");
        require(fs::exists(root/"tail64.png")==tail_ok,"tail64 output existence mismatch");
        records<<thumbgen::Json({{"case","tail64-engine"},{"ok",tail_ok},
            {"error_code",tail_result.contains("error")?tail_result["error"].value("code",std::string{}):std::string{}}}).dump()<<'\n';
        auto broken=root/"first-chunk-truncated.exr";fs::copy_file(complete,broken);
        fs::resize_file(broken,first_chunk_midpoint(complete));
        require(!sentinel_guard(broken,false,records),"first chunk midpoint must fail missing subsequent leaders");
        thumbgen::Job bad;bad.input=broken.u8string();bad.options.input_space="linear";bad.options.background="transparent";
        bad.outputs={{(root/"must-not-exist.png").u8string(),"png",19,19,90}};
        auto r=engine.convert(bad);require(!r["ok"].get<bool>(),"truncated payload must fail");
        require(!fs::exists(root/"must-not-exist.png"),"failed read consumed incomplete buffer");
        // At-threshold guard is mandatory: a test suite consisting only of
        // tiny fallbacks must not qualify the uninitialized-allocation route.
        constexpr std::size_t large_samples=std::size_t(3072)*2048*4;
        static_assert(large_samples==thumbgen::detail::exr_nozero_min_samples,
                      "large fixture must hit the exact evidence boundary");
        auto large=root/"large96.exr";fixture(large,layouts[1],false,false,3072,2048);
        {
            auto in=OIIO::ImageInput::open(large.u8string());require(bool(in),"large fixture open");
            auto s=in->spec();
            require(s.width==3072 && s.height==2048 && s.nchannels==4 && s.format==OIIO::TypeDesc::FLOAT &&
                    s.channelnames==std::vector<std::string>({"R","G","B","A"}) && s.alpha_channel==3,
                    "large canonical FLOAT RGBA fixture dimensions/layout");
            auto plan=thumbgen::detail::channel_plan(s,{0,1,2,3},false);
            require(plan.runs.size()==1 && plan.channels.size()==4,"large fixture must use single dense run");
            in->close();
        }
        require(sentinel_guard(large,false,records),"large complete destination depends on sentinel prefill");
        thumbgen::Job large_job;large_job.id="large96";large_job.input=large.u8string();
        large_job.options.input_space="linear";large_job.options.background="transparent";large_job.options.threads=1;
        for(int size:{19,37})large_job.outputs.push_back({(root/("large96-"+std::to_string(size)+".png")).u8string(),"png",size,size,90});
        auto large_result=engine.convert(large_job);require(large_result["ok"].get<bool>(),large_result.dump());
        require(large_result["decoded_mib"].get<double>()==96.0,"large selected read buffer must be 96 MiB");
        check_nozero_marker(large_result,true);
        records<<thumbgen::Json({{"id","large96"},{"ok",large_result["ok"]},{"timings_ms",large_result["timings_ms"]}}).dump()<<'\n';
        std::cout<<"EXR pipeline synthetic coverage passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
