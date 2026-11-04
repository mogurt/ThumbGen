#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static void write_exr(const fs::path& path,const OIIO::ImageSpec& spec) {
    auto out=OIIO::ImageOutput::create(path.u8string());require(bool(out),OIIO::geterror());
    require(out->open(path.u8string(),spec),out->geterror());
    std::vector<float> pixels(size_t(spec.width)*spec.height*spec.nchannels,1.0f);
    require(out->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),"cannot write EXR fixture");
    require(out->close(),"cannot close EXR fixture");
}
int main() {
    auto root=fs::temp_directory_path()/fs::path("thumbgen-core-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,ec);}} cleanup{root};
    try {
        thumbgen::Engine engine;
        OIIO::ImageSpec cropped(1,1,3,OIIO::TypeDesc::FLOAT);
        cropped.x=2;cropped.y=1;cropped.full_x=0;cropped.full_y=0;cropped.full_width=4;cropped.full_height=3;
        write_exr(root/"crop.exr",cropped);
        thumbgen::Job job;
        job.id="cropped";job.input=(root/"crop.exr").u8string();job.options.preset="fast";
        job.options.input_space="linear";
        job.outputs.push_back({(root/"crop.png").u8string(),"png",4,3,90});
        auto r=engine.convert(job);require(r["ok"].get<bool>(),r.dump());
        OIIO::ImageBuf image((root/"crop.png").u8string());require(image.read(),image.geterror());
        for(int y=0;y<3;++y)for(int x=0;x<4;++x){float pixel[3];image.getpixel(x,y,pixel,3);
            float expected=x==2&&y==1?1.0f:0.0f;require(std::abs(pixel[0]-expected)<0.01f,"cropped canvas not preserved");}

        auto path=(root/"parts.exr").u8string();
        OIIO::ImageSpec parts[2]={OIIO::ImageSpec(2,2,3,OIIO::TypeDesc::FLOAT),OIIO::ImageSpec(2,2,3,OIIO::TypeDesc::FLOAT)};
        parts[0].attribute("oiio:subimagename","first");parts[1].attribute("oiio:subimagename","second");
        auto out=OIIO::ImageOutput::create(path);require(bool(out),OIIO::geterror());
        require(out->open(path,2,parts),out->geterror());float pixels[12]={1,0,0,1,0,0,1,0,0,1,0,0};
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"first part write failed");
        require(out->open(path,parts[1],OIIO::ImageOutput::AppendSubimage),out->geterror());
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"second part write failed");require(out->close(),"close failed");
        job.input=path;job.outputs[0].path=(root/"part.png").u8string();
        auto ambiguous=engine.convert(job);require(!ambiguous["ok"].get<bool>() && ambiguous["error"]["code"]=="ambiguous_parts",ambiguous.dump());
        job.options.part=1;
        auto explicit_part=engine.convert(job);require(explicit_part["ok"].get<bool>() && explicit_part["source"]["part"]==1,explicit_part.dump());
        // An ambiguous first part must not be silently discarded in favour of
        // a later part that happens to contain a single RGB group.
        path=(root/"multi-group-parts.exr").u8string();
        parts[0]=OIIO::ImageSpec(2,2,6,OIIO::TypeDesc::FLOAT);
        parts[0].channelnames={"Diffuse.R","Diffuse.G","Diffuse.B","Specular.R","Specular.G","Specular.B"};
        parts[0].attribute("oiio:subimagename","layers");
        out=OIIO::ImageOutput::create(path);require(bool(out),OIIO::geterror());
        require(out->open(path,2,parts),out->geterror());std::vector<float> multiple(24,0.5f);
        require(out->write_image(OIIO::TypeDesc::FLOAT,multiple.data()),"group part write failed");
        require(out->open(path,parts[1],OIIO::ImageOutput::AppendSubimage),out->geterror());
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"second group part write failed");require(out->close(),"close failed");
        job.input=path;job.options.part=-1;job.outputs[0].path=(root/"groups.png").u8string();
        auto groups=engine.convert(job);require(groups["error"]["code"]=="ambiguous_parts",groups.dump());
        require(groups["selection"]["candidates"].size()==3,groups.dump());
        job.options.part=0;
        auto within=engine.convert(job);require(within["error"]["code"]=="ambiguous_channels",within.dump());
        job.options.channels="Diffuse";
        auto selected=engine.convert(job);require(selected["ok"].get<bool>(),selected.dump());
        std::cout<<"cropped display window and multipart selection passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
