#include "../src/exr_rows.h"
#include <thumbgen/core.h>
#include <OpenEXR/openexr.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs=std::filesystem;
using OIIO::ImageInput;
using OIIO::ImageOutput;
using OIIO::ImageSpec;
using OIIO::TypeDesc;
using thumbgen::detail::probe_exr_rows;

static void require(bool ok,const std::string& message) {
    if(!ok)throw std::runtime_error(message);
}

static void fixture(const fs::path& path,const char* compression,bool tiled,bool half,bool multipart=false) {
    ImageSpec s(19,17,5,half?TypeDesc::HALF:TypeDesc::FLOAT);
    s.channelnames={"beauty.R","beauty.G","beauty.B","beauty.A","Z"};
    s.alpha_channel=3;
    s.z_channel=4;
    s.channelformats={s.format,s.format,s.format,s.format,TypeDesc::FLOAT};
    s.x=-14;s.y=-6;
    s.full_x=-4;s.full_y=-10;s.full_width=27;s.full_height=22;
    if(std::string_view(compression)=="zips") {
        s.x+=100;s.full_x+=100;
        s.y+=100;s.full_y+=100;
    }
    s.attribute("compression",compression);
    s.attribute("Orientation",6);
    if(tiled){s.tile_width=8;s.tile_height=8;s.tile_depth=1;}
    std::vector<float> pixels(std::size_t(s.width)*s.height*s.nchannels);
    for(int y=0;y<s.height;++y)for(int x=0;x<s.width;++x) {
        auto* p=pixels.data()+(y*s.width+x)*s.nchannels;
        const float alpha=float(y+x+2)/40;
        p[0]=alpha*float(x+4)/40;
        p[1]=alpha*float(y+5)/40;
        p[2]=alpha*float(x+y+8)/59;
        p[3]=alpha;
        p[4]=float(x-y);
    }
    auto writer=ImageOutput::create(path.u8string());
    require(bool(writer),"create OpenEXR writer");
    if(multipart) {
        ImageSpec parts[2]={s,s};
        parts[0].attribute("oiio:subimagename","first");
        parts[1].attribute("oiio:subimagename","second");
        const std::vector<float> blank(pixels.size());
        require(writer->open(path.u8string(),2,parts),writer->geterror());
        require(writer->write_image(TypeDesc::FLOAT,blank.data()),writer->geterror());
        require(writer->open(path.u8string(),parts[1],ImageOutput::AppendSubimage),writer->geterror());
    } else require(writer->open(path.u8string(),s),writer->geterror());
    require(writer->write_image(TypeDesc::FLOAT,pixels.data()),writer->geterror());
    require(writer->close(),writer->geterror());
}

static void compare_rows(const fs::path& path,bool tiled,bool half,bool supported,int part=0) {
    auto input=ImageInput::open(path.u8string());
    require(bool(input),"open OpenEXR fixture");
    ImageSpec spec;
    require(input->seek_subimage(part,0,spec),"seek selected part");
    const auto names=std::vector<std::string>{"beauty.B","beauty.R","beauty.G","beauty.A"};
    std::vector<int> channels;
    for(const auto& name:names) {
        auto found=std::find(spec.channelnames.begin(),spec.channelnames.end(),name);
        require(found!=spec.channelnames.end(),"missing test channel");
        channels.push_back(int(found-spec.channelnames.begin()));
    }
    auto probe=probe_exr_rows(path.u8string(),spec,part,channels);
    require(bool(probe)==supported,"unexpected OpenEXR route");
    if(!supported)return;
    const auto sample_bytes=half?2u:4u;
    require(probe->read.pixel_bytes==4*sample_bytes && probe->sample_format==(half?TypeDesc::HALF:TypeDesc::FLOAT),
        "selected native layout");
    auto compression=spec.get_string_attribute("compression");
    const auto strip_rows=tiled?8:compression=="zip"?std::min(16,spec.height):compression=="piz"?spec.height:1;
    const auto strip_width=tiled?spec.width-spec.tile_width:spec.width;
    require(probe->read.retained_bytes>=std::size_t(strip_width)*strip_rows*4*sample_bytes,
        "selected strip storage is charged");
    const auto chunk_pixels=tiled?64:std::size_t(spec.width)*strip_rows;
    const auto all_lane_bytes=chunk_pixels*(4*sample_bytes+4);
    const auto copies=compression=="none"?1:3;
    require(probe->read.codec_bytes>=8*thumbgen::detail::bounded_mib+copies*all_lane_bytes,
        "all source lanes are charged in codec scratch");

    std::vector<float> reference(std::size_t(spec.width)*spec.height*spec.nchannels);
    require(input->read_image(part,0,0,spec.nchannels,TypeDesc::FLOAT,reference.data()),input->geterror());
    input->close();
    auto rows=probe->read.open();
    std::vector<unsigned char> native(std::size_t(spec.full_width)*3*probe->read.pixel_bytes,0xff);
    std::vector<float> converted(std::size_t(spec.full_width)*3*channels.size());
    for(int y0=0;y0<spec.full_height;y0+=3) {
        const int y1=std::min(y0+3,spec.full_height);
        rows->read(y0,y1,native.data());
        require(OIIO::convert_pixel_values(probe->sample_format,native.data(),TypeDesc::FLOAT,converted.data(),
            (y1-y0)*spec.full_width*4),"convert native read");
        for(int y=y0;y<y1;++y)for(int x=0;x<spec.full_width;++x) {
            const auto dx=x+spec.full_x-spec.x,dy=y+spec.full_y-spec.y;
            auto index=std::size_t((y-y0)*spec.full_width+x)*4;
            for(int c=0;c<4;++c) {
                float expected=0;
                if(dx>=0 && dx<spec.width && dy>=0 && dy<spec.height)
                    expected=reference[(dy*spec.width+dx)*spec.nchannels+channels[c]];
                require(converted[index+c]==expected,"shifted native sample differs from full read");
            }
        }
    }
    rows->finish();
}

static void incomplete_offsets(const fs::path& path) {
    fixture(path,"zip",false,false);
    auto input=ImageInput::open(path.u8string());
    require(bool(input),"open offset fixture");
    const auto spec=input->spec();
    input->close();

    exr_context_t context=nullptr;
    const auto filename=path.u8string();
    require(exr_start_read(&context,filename.c_str(),nullptr)==EXR_ERR_SUCCESS,"open offset metadata");
    std::uint64_t offset=0;
    const auto status=exr_get_chunk_table_offset(context,0,&offset);
    exr_finish(&context);
    require(status==EXR_ERR_SUCCESS && offset>0,"locate offset table");
    std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
    file.seekp(std::streamoff(offset));
    const char zero[8]{};
    file.write(zero,sizeof(zero));
    file.close();
    require(bool(file),"clear first chunk offset");
    require(!probe_exr_rows(filename,spec,0,{0,1,2,3}),"incomplete chunk table must use fallback");
}

static void compare_output(const fs::path& lhs,const fs::path& rhs,int width,int height) {
    auto read=[&](const fs::path& path) {
        ImageSpec options;
        options.attribute("oiio:UnassociatedAlpha",1);
        auto input=ImageInput::open(path.u8string(),&options);
        require(bool(input),"open paired preview");
        const auto& s=input->spec();
        require(s.width==width && s.height==height && s.nchannels==4,"oriented output layout");
        std::vector<unsigned char> pixels(std::size_t(width)*height*4);
        require(input->read_image(0,0,0,4,TypeDesc::UINT8,pixels.data()),input->geterror());
        return pixels;
    };
    const auto a=read(lhs),b=read(rhs);
    for(std::size_t i=0;i<a.size();++i)
        require(std::abs(int(a[i])-int(b[i]))<=2,"paired preview maximum error exceeds two code values");
}

class OcioEnvironment {
    std::optional<std::string> previous_;
    static int set(const char* value) {
#ifdef _WIN32
        return _putenv_s("OCIO",value?value:"");
#else
        return value?setenv("OCIO",value,1):unsetenv("OCIO");
#endif
    }
public:
    explicit OcioEnvironment(const std::string& value) {
        if(const char* previous=std::getenv("OCIO"))previous_=previous;
        require(set(value.c_str())==0,"set project OCIO environment");
    }
    ~OcioEnvironment(){set(previous_?previous_->c_str():nullptr);}
};

static void compare_engine(const fs::path& root,const fs::path& input,bool expect_bounded,int part=0) {
    const auto config=root/"test.ocio";
    std::ofstream(config)<<R"(ocio_profile_version: 2
search_path: ''
roles:
  default: workingRGB
  scene_linear: workingRGB
file_rules:
  - !<Rule> {name: Default, colorspace: workingRGB}
displays:
  testDisplay:
    - !<View> {name: testView, colorspace: sRGB}
colorspaces:
  - !<ColorSpace>
    name: workingRGB
    bitdepth: 32f
    isdata: false
  - !<ColorSpace>
    name: encodedInput
    bitdepth: 32f
    isdata: false
    to_scene_reference: !<ExponentTransform> {value: [2, 2, 2, 1]}
  - !<ColorSpace>
    name: sRGB
    bitdepth: 32f
    isdata: false
    from_scene_reference: !<ExponentTransform> {value: [0.5, 0.5, 0.5, 1]}
)";
    thumbgen::Engine bounded,reference;
    bool observed=false;
    thumbgen::Engine::Control control;
    control.planned=[&](const auto&,const auto& report) {
        const bool using_rows=!report.at("footprint").at("bounded_read").is_null();
        require(using_rows==expect_bounded,"unexpected admitted preview route");
        observed=true;
    };
    bounded.set_control(control);
    control.planned={};control.reference_colour=true;
    reference.set_control(control);
    thumbgen::Job job;
    job.input=input.u8string();
    OcioEnvironment environment(config.u8string());
    job.options.input_space="encodedInput";
    job.options.background="transparent";
    job.options.channels="beauty";
    job.options.part=part;
    job.options.threads=1;
    job.sequence_id="synthetic-sequence";
    for(int frame=0;frame<2;++frame) {
        for(int pass=0;pass<2;++pass) {
            job.outputs.clear();
            for(int size:{27,15}) {
                auto path=root/(std::to_string(frame)+"-"+std::to_string(pass)+"-"+std::to_string(size)+".png");
                job.outputs.push_back({path.u8string(),"png",size,size,90});
            }
            auto result=(pass?reference:bounded).convert(job);
            require(result.at("ok").get<bool>(),result.dump());
            require(result.at("selection").at("lock_status")== (frame?"reused":"created"),"sequence lock");
            require(result.at("source").at("part")==part,"selected part index");
            require(result.at("colour").at("config_basis")=="environment","inherit project OCIO");
            require(result.at("colour").at("working_space")=="workingRGB","project OCIO working space");
        }
        compare_output(root/(std::to_string(frame)+"-0-27.png"),root/(std::to_string(frame)+"-1-27.png"),22,27);
        compare_output(root/(std::to_string(frame)+"-0-15.png"),root/(std::to_string(frame)+"-1-15.png"),12,15);
    }
    require(observed,"worker admission was observed");
}

int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: test_bounded_exr EMPTY_OUTPUT_ROOT");
        const fs::path root=fs::absolute(argv[1]);
        require(fs::create_directories(root),"test output root must not exist");
        for(const char* compression:{"none","rle","zips","zip","piz","pxr24"})for(bool tiled:{false,true}) {
            const auto directory=root/(std::string(compression)+(tiled?"-tiled":"-scan"));
            fs::create_directories(directory);
            const auto input=directory/"image.exr";
            const auto name=std::string_view(compression);
            const bool half=tiled || name=="rle";
            const bool supported=name!="pxr24";
            fixture(input,compression,tiled,half);
            compare_rows(input,tiled,half,supported);
            if(name=="zip" || name=="pxr24")compare_engine(directory,input,supported);
        }
        const auto multipart=root/"multipart";
        fs::create_directory(multipart);
        const auto input=multipart/"image.exr";
        fixture(input,"zip",true,true,true);
        compare_rows(input,true,true,true,1);
        compare_engine(multipart,input,true,1);
        incomplete_offsets(root/"incomplete.exr");
        std::cout<<"Bounded flat OpenEXR coverage passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
