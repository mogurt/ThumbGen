#include "data_image.h"
#include <thumbgen/core.h>
#include <OpenImageIO/imagebufalgo.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fs=std::filesystem;
namespace iba=OIIO::ImageBufAlgo;
using namespace thumbgen::detail;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static void check(const OIIO::ImageBuf& image,bool ok){require(ok,image.geterror());}

static void fixture(const fs::path& path,int width,int height) {
    OIIO::ImageSpec spec(width,height,1,OIIO::TypeDesc::UINT32);spec.channelnames={"object_id"};
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror());
    require(output->open(path.u8string(),spec),output->geterror());std::vector<std::uint32_t> row(width);
    const std::uint32_t ids[]={0,1,42,16777216,16777217,4294967295u};
    for(int y=0;y<height;++y) {
        for(int x=0;x<width;++x)row[x]=ids[(x/17+y/13)%6];
        require(output->write_scanline(y,0,OIIO::TypeDesc::UINT32,row.data()),output->geterror());
    }
    require(output->close(),output->geterror());
}

static void compare(const fs::path& path,int orientation,int window,bool rgb) {
    OIIO::ImageSpec spec(17,11,rgb?3:1,rgb?OIIO::TypeDesc::DOUBLE:OIIO::TypeDesc::UINT32);
    spec.channelnames=rgb?std::vector<std::string>{"matID.R","matID.G","matID.B"}:std::vector<std::string>{"object_id"};
    spec.x=-7;spec.y=9;spec.full_x=-10;spec.full_y=7;spec.full_width=23;spec.full_height=19;
    if(window==1){spec.full_x=spec.x+2;spec.full_y=spec.y+1;spec.full_width=13;spec.full_height=7;}
    if(window==2){spec.full_x=spec.x;spec.full_y=spec.y;spec.full_width=spec.width;spec.full_height=spec.height;}
    spec.attribute("Orientation",orientation);
    auto mapping=data_mapping(rgb?"id_rgb":"id",rgb?std::vector<double>{0,1}:std::vector<double>{},"magenta");
    const std::size_t count=std::size_t(spec.width)*spec.height;
    std::vector<double> source(count*spec.nchannels);std::vector<std::uint32_t> ids(count);
    const std::uint32_t values[]={0,1,42,16777216,16777217,4294967295u};
    for(std::size_t p=0;p<count;++p) {
        ids[p]=values[p%6];
        if(rgb){source[p*3]=double(p%7)/6;source[p*3+1]=double(p%5)/4;source[p*3+2]=double(p%3)/2;}
        else source[p]=ids[p];
    }
    if(rgb){source[0]=std::numeric_limits<double>::quiet_NaN();source[5]=2;source[11]=-1;}
    auto writer=OIIO::ImageOutput::create(path.u8string());require(bool(writer),OIIO::geterror());
    require(writer->open(path.u8string(),spec),writer->geterror());
    require(writer->write_image(spec.format,rgb?static_cast<const void*>(source.data()):static_cast<const void*>(ids.data())),writer->geterror());
    require(writer->close(),writer->geterror());
    auto input=OIIO::ImageInput::open(path.u8string());require(bool(input),OIIO::geterror());
    // TIFF may rename channel groups, but component order and native values remain.
    spec=input->spec();std::vector<int> channels=rgb?std::vector<int>{0,1,2}:std::vector<int>{0};
    auto native=read_data_image(*input,spec,0,channels,mapping,16*1024*1024,3);require(input->close(),"close input");
    require(native.native.size()==count*spec.pixel_bytes(true) && !native.image.initialized(),"full mapped image allocated");
    std::vector<float> mapped(count*4);auto stats=map_data_double(source.data(),mapped.data(),count,channels.size(),channels,mapping);
    require(stats.invalid_pixels==native.statistics.invalid_pixels && stats.clipped_components==native.statistics.clipped_components,"full source statistics differ");
    if(rgb && orientation==1 && window==0) {
        auto again=OIIO::ImageInput::open(path.u8string());require(bool(again),OIIO::geterror());
        auto strict=mapping;strict.invalid=InvalidData::error;bool rejected=false;
        try{read_data_image(*again,spec,0,channels,strict,16*1024*1024,3);}
        catch(const InvalidDataValues& error){rejected=error.statistics.invalid_pixels==1;}
        require(again->close(),"close strict input");require(rejected,"unsampled invalid source was ignored");
    }
    auto rgba=spec;rgba.nchannels=4;rgba.set_format(OIIO::TypeDesc::FLOAT);rgba.channelnames={"R","G","B","A"};rgba.alpha_channel=3;
    OIIO::ImageBuf original(rgba,mapped.data()),padded;
    check(padded,iba::crop(padded,original,OIIO::ROI(spec.full_x,spec.full_x+spec.full_width,spec.full_y,spec.full_y+spec.full_height,0,1,0,4)));
    padded.set_origin(0,0,0);padded.set_roi_full(padded.roi());
    OIIO::ImageBuf oriented;check(oriented,iba::reorient(oriented,padded));
    // Independent old pipeline oracle: full map -> crop/pad -> orient -> OIIO nearest.
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{2,3},{7,5},{13,7},{17,11},{23,19}}) {
        OIIO::ImageBuf expected;check(expected,iba::resample(expected,oriented,false,OIIO::ROI(0,size.first,0,size.second,0,1,0,4),3));
        auto actual=categorical_preview(native,size.first,size.second,16*1024*1024,3);
        std::vector<float> a(std::size_t(size.first)*size.second*4),b(a.size());
        require(expected.get_pixels(expected.roi(),OIIO::TypeDesc::FLOAT,a.data()),expected.geterror());
        require(actual.get_pixels(actual.roi(),OIIO::TypeDesc::FLOAT,b.data()),actual.geterror());
        require(std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0,"nearest/orientation/window mismatch: "+std::to_string(orientation)+"/"+std::to_string(window)+(rgb?" RGB":" ID"));
    }
}
static int run(const std::vector<fs::path>& args) {
    try {
        if(args.size()==5 && args[1]=="--fixture") {
            auto width=std::stoi(args[3].string()),height=std::stoi(args[4].string());
            require(width>0 && width<=16384 && height>0 && height<=16384,"invalid fixture dimensions");
            require(!fs::exists(args[2]),"fixture already exists");fixture(args[2],width,height);return 0;
        }
        auto root=fs::temp_directory_path()/("thumbgen-categorical-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root),"cannot create owned fixture directory");
        struct Cleanup{fs::path root;~Cleanup(){std::error_code ec;if(root.parent_path()==fs::temp_directory_path())fs::remove_all(root,ec);}}cleanup{root};
        for(int orientation=1;orientation<=8;++orientation)for(int window=0;window<3;++window)for(bool rgb:{false,true})
            compare(root/(rgb?"rgb.tif":"id.exr"),orientation,window,rgb);
        fixture(root/"large.exr",512,512);
        thumbgen::Job job;job.id="bounded-native";job.input=(root/"large.exr").u8string();
        job.options.visualization="id";job.options.channels="object_id";job.options.max_decode_mib=1;
        job.outputs.push_back({(root/"small.png").u8string(),"png",16,16,90});thumbgen::Engine engine;
        auto result=engine.convert(job);require(result["ok"].get<bool>() && result["decoded_mib"].get<double>()<1 &&
            result["decoded_mib"].get<double>()*1048576==result["source_read"]["buffer_bytes"].get<double>(),result.dump());
        // A sparse EXR display window must not allocate its 16 GiB RGBA canvas.
        OIIO::ImageSpec sparse(2,2,1,OIIO::TypeDesc::UINT32);sparse.channelnames={"object_id"};
        sparse.x=sparse.y=16383;sparse.full_x=sparse.full_y=0;sparse.full_width=sparse.full_height=32768;
        auto writer=OIIO::ImageOutput::create((root/"sparse.exr").u8string());require(bool(writer),OIIO::geterror());
        require(writer->open((root/"sparse.exr").u8string(),sparse),writer->geterror());
        const std::uint32_t labels[]={0,1,16777217,42};
        require(writer->write_image(OIIO::TypeDesc::UINT32,labels),writer->geterror());require(writer->close(),writer->geterror());
        job.input=(root/"sparse.exr").u8string();job.outputs[0]={(root/"sparse.png").u8string(),"png",1,1,90};
        result=engine.convert(job);require(result["ok"].get<bool>(),result.dump());
        OIIO::ImageBuf sparse_output(job.outputs[0].path);require(sparse_output.read(),sparse_output.geterror());
        unsigned char pixel[3]={};require(sparse_output.get_pixels(sparse_output.roi(),OIIO::TypeDesc::UINT8,pixel),sparse_output.geterror());
        require(pixel[0]==213 && pixel[1]==140 && pixel[2]==248,"sparse display window sampled incorrectly");
        std::cout<<"288 old-pipeline pixel comparisons and native-buffer-only limit passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int count,wchar_t** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#else
int main(int count,char** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#endif
