#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    auto root=fs::temp_directory_path()/("thumbgen-fast-colour-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    thumbgen::Engine fast,reference;thumbgen::Engine::Control control;control.reference_colour=true;reference.set_control(control);
    OIIO::ImageSpec spec(127,65,3,OIIO::TypeDesc::UINT8);OIIO::ImageBuf input(spec);
    auto* p=static_cast<unsigned char*>(input.localpixels());
    for(int y=0;y<65;++y)for(int x=0;x<127;++x)for(int c=0;c<3;++c)p[(y*127+x)*3+c]=static_cast<unsigned char>((x*17+y*31+c*61)%256);
    auto source=root/"rgb.png";require(input.write(source.u8string()),input.geterror());
    thumbgen::Job job;job.input=source.u8string();job.options.input_space="sRGB";job.options.threads=1;
    job.outputs={{(root/"fast.png").u8string(),"png",32,32,90},{(root/"fast-small.png").u8string(),"png",13,11,90}};
    auto actual=fast.convert(job);require(actual.value("ok",false),actual.dump());require(actual["colour"]["processing"]=="srgb8-linear-v1",actual.dump());
    job.outputs[0].path=(root/"reference.png").u8string();job.outputs[1].path=(root/"reference-small.png").u8string();
    auto expected=reference.convert(job);require(expected.value("ok",false),expected.dump());require(expected["colour"]["processing"]=="reference",expected.dump());
    for(int i=0;i<2;++i){
        OIIO::ImageBuf a(actual["outputs"][i]["path"].get<std::string>()),b(expected["outputs"][i]["path"].get<std::string>());
        require(a.read(0,0,true,OIIO::TypeDesc::UINT8) && b.read(0,0,true,OIIO::TypeDesc::UINT8),"readback failed");
        require(a.spec().width==b.spec().width && a.spec().height==b.spec().height,"shape mismatch");
        auto* x=static_cast<const unsigned char*>(a.localpixels());auto* y=static_cast<const unsigned char*>(b.localpixels());
        for(std::size_t p=0;p<a.spec().image_bytes();++p)require(std::abs(int(x[p])-int(y[p]))<=1,"fast/reference pixel mismatch");
    }
    job.options.preset="quality";job.outputs.resize(1);job.outputs[0].path=(root/"quality.png").u8string();
    auto quality=fast.convert(job);require(quality.value("ok",false) && quality["colour"]["processing"]=="reference",quality.dump());
    fs::remove_all(root);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
