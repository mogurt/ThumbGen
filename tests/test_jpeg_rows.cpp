#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace {
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
struct Temp {
    fs::path path=fs::temp_directory_path()/("thumbgen-jpeg-rows-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){fs::create_directory(path);}
    ~Temp(){std::error_code error;fs::remove_all(path,error);}
};
void write_jpeg(const fs::path& path,int channels,int orientation){
    OIIO::ImageSpec spec(4097,2001,channels,OIIO::TypeDesc::UINT8);
    spec.attribute("Orientation",orientation);
    auto out=OIIO::ImageOutput::create(path.u8string());
    require(out && out->open(path.u8string(),spec),OIIO::geterror());
    std::vector<unsigned char> row(std::size_t(spec.width)*channels);
    for(int y=0;y<spec.height;++y){
        for(int x=0;x<spec.width;++x)
            for(int c=0;c<channels;++c)
                row[std::size_t(x)*channels+c]=static_cast<unsigned char>((x/11+y/19+c*61)%256);
        require(out->write_scanline(y,0,spec.format,row.data()),out->geterror());
    }
    require(out->close(),out->geterror());
}
void compare(const thumbgen::Json& a,const thumbgen::Json& b) {
    OIIO::ImageBuf x(a.at("path").get<std::string>()),y(b.at("path").get<std::string>());
    require(x.read(0,0,true,OIIO::TypeDesc::UINT8) && y.read(0,0,true,OIIO::TypeDesc::UINT8),"readback failed");
    require(x.spec().width==y.spec().width && x.spec().height==y.spec().height &&
        x.nchannels()==y.nchannels(),"dimensions differ from full decode");
    const auto* xp=static_cast<const unsigned char*>(x.localpixels());
    const auto* yp=static_cast<const unsigned char*>(y.localpixels());
    int max_error=0;
    double abs_error=0;
    for(std::size_t i=0;i<x.spec().image_bytes();++i) {
        const int d=std::abs(int(xp[i])-int(yp[i]));
        max_error=std::max(max_error,d);abs_error+=d;
    }
    require(max_error<=3 && abs_error/x.spec().image_bytes()<0.25,
        "bounded preview error exceeds threshold: max "+std::to_string(max_error)+", mean "+std::to_string(abs_error/x.spec().image_bytes()));
}
}
int main(){try{
    Temp root;
    for(auto [components,orientation]:std::array<std::pair<int,int>,3>{{{3,1},{3,6},{1,8}}}){
        const auto source=root.path/"source.jpg";
        write_jpeg(source,components,orientation);
        thumbgen::Job job;
        job.input=source.u8string();job.options.threads=1;job.options.input_space="sRGB";
        job.outputs={{(root.path/"bounded-large.png").u8string(),"png",641,479,90,"replace"},
            {(root.path/"bounded-small.png").u8string(),"png",173,151,90,"replace"}};
        bool admitted=false;
        int allocations=0,decodes=0;
        thumbgen::Engine engine([&](const std::string& phase,const thumbgen::Job&){
            if(phase=="c2_allocate"){require(admitted,"bounded allocation preceded admission");++allocations;}
            if(phase=="decoded")++decodes;
        });
        thumbgen::Engine::Control control;
        control.planned=[&](const thumbgen::Job& requested,const thumbgen::Json& plan){
            require(plan.at("footprint").at("bounded_read").is_null()==(requested.outputs.size()!=2),plan.dump());
            admitted=true;
        };
        engine.set_control(control);
        const auto actual=engine.convert(job);
        require(actual.value("ok",false) && allocations==1 && decodes==1,actual.dump());
        if(orientation>=5)require(actual["outputs"][0]["width"]<actual["outputs"][0]["height"],actual.dump());
        job.outputs[0].path=(root.path/"full-large.png").u8string();
        job.outputs[1].path=(root.path/"full-small.png").u8string();
        // Three native callers' outputs exercise the existing full-source path.
        job.outputs.push_back({(root.path/"full-third.png").u8string(),"png",64,64,90,"replace"});
        const auto expected=engine.convert(job);
        require(expected.value("ok",false) && allocations==1 && decodes==2,expected.dump());
        for(int i=0;i<2;++i)compare(actual["outputs"][i],expected["outputs"][i]);
    }
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
