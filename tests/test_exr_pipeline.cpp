// Synthetic inputs only. Run the same executable in baseline/candidate builds,
// separate roots, then compare the PNGs exactly (and success/failure records).
#include <thumbgen/core.h>
#include <OpenImageIO/imageio.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
static void require(bool v,const std::string& s){if(!v)throw std::runtime_error(s);}
static void fixture(const fs::path& path,const std::vector<std::string>& names,bool tiled,bool half) {
    OIIO::ImageSpec s(73,41,int(names.size()),half?OIIO::TypeDesc::HALF:OIIO::TypeDesc::FLOAT);
    s.channelnames=names;s.alpha_channel=-1;
    for(int c=0;c<s.nchannels;++c)if(names[c]=="A")s.alpha_channel=c;
    s.attribute("compression","none");s.attribute("oiio:ColorSpace","linear");
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
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: test_exr_pipeline EMPTY_OUTPUT_ROOT");fs::path root=fs::absolute(argv[1]);
        require(!fs::exists(root),"test output root must not exist");fs::create_directories(root);
        const std::vector<std::vector<std::string>> layouts={{"R","G","B"},{"R","G","B","A"},{"Y"},{"A","B","aux.X","G","R"}};
        thumbgen::Engine engine;std::ofstream records(root/"records.jsonl");
        for(std::size_t i=0;i<layouts.size();++i)for(int tiled=0;tiled<2;++tiled) {
            auto id=std::to_string(i)+"-"+std::to_string(tiled);auto source=root/(id+".exr");fixture(source,layouts[i],bool(tiled),i%2==0);
            thumbgen::Job job;job.id=id;job.input=source.u8string();job.options.input_space="linear";job.options.background="transparent";job.options.threads=1;
            for(int size:{19,37})job.outputs.push_back({(root/(id+"-"+std::to_string(size)+".png")).u8string(),"png",size,size,90});
            auto r=engine.convert(job);require(r["ok"].get<bool>(),r.dump());
            for(auto& x:r["timings_ms"].items())require(x.value().is_number()&&x.value().get<double>()>=0,"numeric timings contract");
            // Do not write absolute paths in the portable comparison records.
            records<<thumbgen::Json({{"id",id},{"ok",r["ok"]},{"timings_ms",r["timings_ms"]}}).dump()<<'\n';
        }
        auto broken=root/"truncated.exr";fixture(broken,layouts[1],false,false);
        auto n=fs::file_size(broken);require(n>128,"fixture length");fs::resize_file(broken,n-64);
        thumbgen::Job bad;bad.input=broken.u8string();bad.options.input_space="linear";bad.options.background="transparent";
        bad.outputs={{(root/"must-not-exist.png").u8string(),"png",19,19,90}};
        auto r=engine.convert(bad);require(!r["ok"].get<bool>(),"truncated payload must fail");
        require(!fs::exists(root/"must-not-exist.png"),"failed read consumed incomplete buffer");
        std::cout<<"EXR pipeline synthetic coverage passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
