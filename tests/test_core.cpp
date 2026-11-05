#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
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
static std::vector<unsigned char> bytes(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    require(bool(in),"cannot read encoded output");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
static void jpeg_quality(const fs::path& path,int quality) {
    // Independently inspect the actual JPEG luminance quantization table,
    // rather than trusting requested metadata or a file size comparison.
    const unsigned char standard[]={16,11,12,14,12,10,16,14,13,14,18,17,16,19,24,40,
        26,24,22,22,24,49,35,37,29,40,58,51,61,60,57,51,56,55,64,72,92,78,64,68,
        87,69,55,56,80,109,81,87,95,98,103,104,103,62,77,113,121,112,100,120,92,101,103,99};
    auto data=bytes(path);require(data.size()>4 && data[0]==255 && data[1]==216,"JPEG header");
    for(size_t pos=2;pos+4<=data.size();) {
        require(data[pos]==255,"JPEG marker boundary");auto marker=data[pos+1];
        if(marker==0xda || marker==0xd9)break;
        size_t length=(size_t(data[pos+2])<<8)|data[pos+3];
        require(length>=2 && pos+2+length<=data.size(),"JPEG segment length");
        if(marker==0xdb)for(size_t q=pos+4;q<pos+2+length;) {
            auto info=data[q++];require((info>>4)<=1,"JPEG DQT precision");
            size_t size=(info>>4)?128:64;require(q+size<=pos+2+length,"JPEG DQT bounds");
            if((info&15)==0) {
                require(size==64,"expected 8-bit luminance table");
                int scale=quality<50?5000/quality:200-2*quality;
                for(size_t i=0;i<64;++i)require(data[q+i]==std::max(1,std::min(255,(standard[i]*scale+50)/100)),"requested JPEG quality ignored");
                return;
            }
            q+=size;
        }
        pos+=2+length;
    }
    require(false,"missing JPEG luminance table");
}
static void output_compression(const fs::path& root,thumbgen::Engine& engine) {
    const char* extensions[]={"exr","tif","png","webp"};
    const char* compressions[]={"zip","lzw","zip","lossless"};
    std::vector<float> pixels(32*16*3);
    for(size_t i=0;i<pixels.size();++i)pixels[i]=float((i*37)%251)/250.0f;
    for(int source=0;source<4;++source) {
        auto input=root/(std::string("compression-source.")+extensions[source]);
        OIIO::ImageSpec spec(32,16,3,source==0?OIIO::TypeDesc::FLOAT:OIIO::TypeDesc::UINT8);
        spec.attribute("compression",compressions[source]);spec.attribute("CompressionQuality",100);
        auto out=OIIO::ImageOutput::create(input.u8string());require(bool(out),OIIO::geterror());
        require(out->open(input.u8string(),spec),out->geterror());
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),"compression fixture write");
        require(out->close(),"compression fixture close");
        thumbgen::Job job;job.input=input.u8string();job.options.input_space=source==0?"linear":"sRGB";
        for(int quality:{20,90}) {
            auto stem=std::string("compression-")+extensions[source]+"-"+std::to_string(quality);
            job.outputs={{(root/(stem+".jpg")).u8string(),"jpeg",32,16,quality},
                         {(root/(stem+".webp")).u8string(),"webp",32,16,quality}};
            auto result=engine.convert(job);require(result["ok"].get<bool>(),result.dump());
            jpeg_quality(root/(stem+".jpg"),quality);
            auto webp=bytes(root/(stem+".webp"));bool lossy=false;
            for(size_t p=12;p+8<=webp.size();) {
                size_t n=size_t(webp[p+4])|(size_t(webp[p+5])<<8)|(size_t(webp[p+6])<<16)|(size_t(webp[p+7])<<24);
                require(n<=webp.size()-p-8,"WebP chunk bounds");
                std::string tag(webp.begin()+p,webp.begin()+p+4);
                require(tag!="VP8L","source lossless mode leaked into WebP preview");lossy=lossy || tag=="VP8 ";
                p+=8+n+(n&1);
            }
            require(lossy,"missing lossy WebP payload");
        }
    }
}
static void quantization_roundtrip(const fs::path& root,thumbgen::Engine& engine) {
    // Two distinct named spaces with no transform make a linear identity
    // config: test quantization without a project-specific colour transform.
    auto config=root/"quantization.ocio";
    std::ofstream(config)<<R"(ocio_profile_version: 2
search_path: ''
strictparsing: true
roles:
  default: workingRGB
  scene_linear: workingRGB
file_rules:
  - !<Rule> {name: Default, colorspace: workingRGB}
displays:
  test:
    - !<View> {name: test, colorspace: sRGB}
colorspaces:
  - !<ColorSpace> {name: workingRGB, bitdepth: 32f, isdata: false, allocation: uniform}
  - !<ColorSpace> {name: sRGB, bitdepth: 32f, isdata: false, allocation: uniform}
)";
    for(int channels:{3,4}) {
        OIIO::ImageSpec spec(425,239,channels,OIIO::TypeDesc::FLOAT);
        spec.attribute("DateTime","2000:01:01 00:00:00");
        std::vector<float> pixels(size_t(425)*239*channels);
        std::vector<unsigned char> expected(pixels.size());
        for(size_t i=0;i<pixels.size();++i) {
            pixels[i]=channels==4 && i%4==3?1.0f:(float(i%255)+0.5f)/255.0f;
            const float scaled=pixels[i]*255.0f;
            expected[i]=static_cast<unsigned char>(std::floor(double(scaled)+0.5));
        }
        auto input=root/("quantization-"+std::to_string(channels)+".exr");
        auto out=OIIO::ImageOutput::create(input.u8string());require(bool(out),OIIO::geterror());
        require(out->open(input.u8string(),spec),out->geterror());
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),"quantization fixture write");
        require(out->close(),"quantization fixture close");
        std::vector<std::vector<unsigned char>> baseline;
        for(int threads:{1,2,4,8}) {
            thumbgen::Job job;job.input=input.u8string();job.options.input_space="workingRGB";
            job.options.preset="fast"; // Box at 1:1 isolates quantization from triangle arithmetic.
            job.options.ocio_config=config.u8string();job.options.threads=threads;
            for(const char* format:{"jpeg","png","webp"}) {
                auto name="quantization-"+std::to_string(channels)+"-"+std::to_string(threads)+"."+format;
                job.outputs.push_back({(root/name).u8string(),format,425,239,90});
            }
            const auto all_outputs=job.outputs;
            if(channels==4) {
                // JPEG requires an opaque background; the other encoders use
                // transparent mode in a separate job with the same source.
                auto jpeg=job; jpeg.outputs.resize(1);
                auto result=engine.convert(jpeg);require(result["ok"].get<bool>(),result.dump());
                job.outputs.erase(job.outputs.begin());job.options.background="transparent";
            }
            auto result=engine.convert(job);require(result["ok"].get<bool>(),result.dump());
            for(size_t i=0;i<all_outputs.size();++i) {
                auto data=bytes(fs::u8path(all_outputs[i].path));
                if(threads==1)baseline.push_back(data);
                else require(data==baseline[i],"encoded output changed with thread count");
                if(all_outputs[i].format=="png") {
                    auto reader=OIIO::ImageInput::open(all_outputs[i].path);require(bool(reader),OIIO::geterror());
                    require(reader->spec().format==OIIO::TypeDesc::UINT8,"PNG is not 8-bit");
                    require(reader->spec().width==425 && reader->spec().height==239 && reader->spec().nchannels==channels,"PNG quantization shape mismatch");
                    std::vector<unsigned char> actual(expected.size());
                    require(reader->read_image(0,0,0,channels,OIIO::TypeDesc::UINT8,actual.data()),reader->geterror());
                    require(reader->close(),"quantization PNG close");
                    if(actual!=expected) {
                        auto mismatch=std::mismatch(actual.begin(),actual.end(),expected.begin());
                        auto index=static_cast<size_t>(mismatch.first-actual.begin());
                        throw std::runtime_error("PNG quantization mismatch: channels="+std::to_string(channels)+
                            " threads="+std::to_string(threads)+" index="+std::to_string(index)+
                            " input="+std::to_string(pixels[index])+" expected="+std::to_string(expected[index])+
                            " actual="+std::to_string(actual[index]));
                    }
                }
            }
        }
    }
}
static void opaque_lanczos_background(const fs::path& root,thumbgen::Engine& engine) {
    OIIO::ImageSpec spec(8,2,4,OIIO::TypeDesc::FLOAT);spec.channelnames={"R","G","B","A"};spec.alpha_channel=3;
    spec.attribute("oiio:ColorSpace","linear");
    std::vector<float> pixels(8*2*4,0);
    for(int y=0;y<2;++y)pixels[(y*8+3)*4+3]=1;
    auto source=(root/"lanczos-alpha.exr").u8string();
    auto writer=OIIO::ImageOutput::create(source);require(bool(writer),OIIO::geterror());
    require(writer->open(source,spec),writer->geterror());require(writer->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),writer->geterror());
    require(writer->close(),writer->geterror());
    for(const std::string background:{"white","black","transparent"}) {
        thumbgen::Job job;job.id="opaque-lanczos";job.input=source;job.options.preset="quality";
        job.options.input_space="linear";job.options.ocio_config="ocio://default";job.options.background=background;
        auto path=root/("lanczos-"+background+".png");job.outputs.push_back({path.u8string(),"png",4,1,90});
        auto result=engine.convert(job);require(result["ok"].get<bool>(),result.dump());
        auto input=OIIO::ImageInput::open(path.u8string());require(bool(input),OIIO::geterror());
        const int bands=background=="transparent"?4:3;
        require(input->spec().width==4 && input->spec().height==1 && input->spec().nchannels==bands,"Lanczos background dimensions");
        std::vector<unsigned char> actual(4*bands);
        require(input->read_image(0,0,0,bands,OIIO::TypeDesc::UINT8,actual.data()),input->geterror());require(input->close(),"close Lanczos output");
        // The alpha impulse has negative Lanczos lobes at both outside pixels.
        // A fully opaque white background must remain white there, regardless
        // of a nonphysical filtered alpha. Black RGB remains black otherwise.
        for(int x:{0,3})for(int c=0;c<3;++c)
            require(actual[x*bands+c]==(background=="white"?255:0),"opaque background darkened by negative Lanczos alpha");
        if(bands==4)require(actual[3]==0 && actual[3*bands+3]==0,"negative alpha did not clamp at storage");
    }
}
int main() {
    auto root=fs::temp_directory_path()/fs::path("thumbgen-core-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,ec);}} cleanup{root};
    try {
        thumbgen::Engine engine;
        output_compression(root,engine);
        opaque_lanczos_background(root,engine);
        quantization_roundtrip(root,engine);
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
        // Named part identity survives part order changes in a sequence.
        thumbgen::Engine sequence_engine;
        for(int frame=0;frame<2;++frame) {
            OIIO::ImageSpec beauty(2,2,3,OIIO::TypeDesc::FLOAT),normal=beauty;
            beauty.attribute("oiio:subimagename","Beauty");normal.attribute("oiio:subimagename","normal");
            OIIO::ImageSpec pair[2]={frame?normal:beauty,frame?beauty:normal};
            path=(root/("ordered-"+std::to_string(frame)+".exr")).u8string();
            out=OIIO::ImageOutput::create(path);require(bool(out),OIIO::geterror());
            require(out->open(path,2,pair),out->geterror());require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"ordered first part");
            require(out->open(path,pair[1],OIIO::ImageOutput::AppendSubimage),out->geterror());
            require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"ordered second part");require(out->close(),"ordered close");
            job.options.part=-1;job.options.part_name="Beauty";job.options.channels.clear();job.sequence_id="ordered";
            job.input=path;job.outputs[0].path=(root/("ordered-"+std::to_string(frame)+".png")).u8string();
            auto result=sequence_engine.convert(job);
            require(result["ok"].get<bool>() && result["selection"]["chosen"]["part_name"]=="Beauty" && result["source"]["part"]==frame,result.dump());
            require(result["selection"]["lock_status"]==(frame?"reused":"created"),result.dump());
        }
        // An unnamed multipage format has no stable semantic part identity.
        path=(root/"unnamed-pages.tif").u8string();
        OIIO::ImageSpec page(2,2,3,OIIO::TypeDesc::UINT8);
        out=OIIO::ImageOutput::create(path);require(bool(out),OIIO::geterror());
        require(out->open(path,page),out->geterror());require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"first TIFF page");
        require(out->open(path,page,OIIO::ImageOutput::AppendSubimage),out->geterror());
        require(out->write_image(OIIO::TypeDesc::FLOAT,pixels),"second TIFF page");require(out->close(),"TIFF close");
        job.input=path;job.options.part=0;job.options.part_name.clear();job.sequence_id="unnamed";
        job.outputs[0].path=(root/"unnamed.png").u8string();
        auto unnamed=sequence_engine.convert(job);require(unnamed["error"]["code"]=="sequence_part_identity_required",unnamed.dump());
        std::cout<<"cropped display window and multipart selection passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
