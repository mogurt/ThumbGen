#include "selected_input.h"
#include "data_image.h"
#include <thumbgen/core.h>
#include <OpenImageIO/half.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace thumbgen::detail;
static void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
template<class T> static void store(unsigned char* p,T value){std::memcpy(p,&value,sizeof(value));}
template<class T> static double number(const unsigned char* p){T value;std::memcpy(&value,p,sizeof(value));return double(value);}
static double native_number(const unsigned char* p,OIIO::TypeDesc type) {
    if(type==OIIO::TypeDesc::HALF)return number<half>(p);
    if(type==OIIO::TypeDesc::FLOAT)return number<float>(p);
    require(type==OIIO::TypeDesc::UINT32,"unexpected fixture type");return number<std::uint32_t>(p);
}
static void fixture(const fs::path& path,int width,int height,int count,int tile,const std::string& compression,bool exceptional=false) {
    require(width>0 && height>0 && count>=8 && count<=256,"invalid fixture dimensions");
    OIIO::ImageSpec spec(width,height,count,OIIO::TypeDesc::FLOAT);spec.channelnames.clear();spec.channelformats.clear();
    spec.attribute("compression",compression);spec.attribute("DateTime","2000:01:01 00:00:00");spec.attribute("oiio:ColorSpace","linear");
    spec.x=spec.full_x=-7;spec.y=spec.full_y=-13;
    for(int c=0;c<count;++c) {
        spec.channelnames.push_back(c==0?"A":c>=count-3?std::string("beauty.")+"RGB"[c-count+3]:"aaa"+std::to_string(1000+c)+".X");
        spec.channelformats.push_back(c==count-3?OIIO::TypeDesc::HALF:c==count-1?OIIO::TypeDesc::UINT32:OIIO::TypeDesc::FLOAT);
    }
    spec.alpha_channel=0;if(tile){spec.tile_width=spec.tile_height=tile;spec.tile_depth=1;}
    const auto stride=spec.pixel_bytes(true),pixels=std::size_t(width)*height;
    require(pixels<=256ULL*1024*1024/stride,"fixture exceeds 256 MiB");
    std::vector<unsigned char> data(pixels*stride);
    const std::uint32_t ids[]={0,1,42,16777216,16777217,4294967295u};
    for(std::size_t p=0;p<pixels;++p) {
        auto* target=data.data()+p*stride;
        for(int c=0;c<count;++c) {
            auto type=spec.channelformat(c);
            if(type==OIIO::TypeDesc::HALF)store(target,half(float(p%9)/8));
            else if(type==OIIO::TypeDesc::UINT32)store(target,ids[p%6]);
            else {
                float value=c==0?(p%3?0.5f:1.0f):float((p*17+std::size_t(c)*13)%251)/250;
                if(exceptional && c==0 && p%std::size_t(width)==0) {
                    auto y=p/std::size_t(width);
                    if(y==31 || y==32 || y==64 || y==136)value=std::numeric_limits<float>::quiet_NaN();
                    else value=2;
                }
                store(target,value);
            }
            target+=type.size();
        }
    }
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror());
    require(output->open(path.u8string(),spec),output->geterror());
    require(output->write_image(OIIO::TypeDesc::UNKNOWN,data.data()),output->geterror());require(output->close(),output->geterror());
}
static int channel(const OIIO::ImageSpec& spec,const char* name) {
    auto found=std::find(spec.channelnames.begin(),spec.channelnames.end(),name);require(found!=spec.channelnames.end(),"missing fixture channel");
    return int(found-spec.channelnames.begin());
}
static void compare(const fs::path& path,bool exceptional=false) {
    auto input=OIIO::ImageInput::open(path.u8string());require(bool(input),OIIO::geterror());auto spec=input->spec();
    auto pixels=std::size_t(spec.width)*spec.height;
    std::vector<float> complete(pixels*spec.nchannels);
    require(input->read_image(0,0,0,spec.nchannels,OIIO::TypeDesc::FLOAT,complete.data()),input->geterror());
    auto stride=spec.pixel_bytes(true);std::vector<unsigned char> original(pixels*stride);
    require(input->read_image(0,0,0,spec.nchannels,OIIO::TypeDesc::UNKNOWN,original.data()),input->geterror());
    std::vector<std::size_t> offsets(spec.nchannels);std::size_t offset=0;
    for(int c=0;c<spec.nchannels;++c){offsets[c]=offset;offset+=spec.channelformat(c).size();}
    int a=channel(spec,"A"),r=channel(spec,"beauty.R"),g=channel(spec,"beauty.G"),b=channel(spec,"beauty.B");
    require(std::max({a,r,g,b})-std::min({a,r,g,b})+1==spec.nchannels,"fixture sparse span collapsed");
    for(const auto& selected:std::vector<std::vector<int>>{{r,g,b,a},{b,a,r},{g,g,a},{b},{g,r},{a,b}}) {
        auto colour=channel_plan(spec,selected,false);std::vector<float> packed(pixels*colour.channels.size());
        require(read_selected(*input,spec,0,colour,false,packed.data()),input->geterror());
        for(std::size_t p=0;p<pixels;++p)for(std::size_t c=0;c<colour.channels.size();++c)
            require(std::memcmp(&packed[p*colour.channels.size()+c],&complete[p*spec.nchannels+colour.channels[c]],sizeof(float))==0,"compact FLOAT differs from full span");
        auto native=channel_plan(spec,selected,true);std::vector<unsigned char> raw(pixels*native.pixel_bytes);
        require(read_selected(*input,spec,0,native,true,raw.data()),input->geterror());
        for(std::size_t p=0;p<pixels;++p)for(std::size_t c=0;c<native.channels.size();++c) {
            auto source=native.channels[c];require(std::memcmp(raw.data()+native.offsets[c]+p*native.strides[c],original.data()+p*stride+offsets[source],spec.channelformat(source).size())==0,"compact native bits differ from full span");
        }
    }
    // Exercise native plane addressing through the actual data mapper, not just the reader.
    auto mapping=data_mapping("vector",{0,1},exceptional?"magenta":"error");
    for(const auto& selected:std::vector<std::vector<int>>{{b,a},{r,g,b},{g,g,a}}) {
    auto image=read_data_image(*input,spec,0,selected,mapping,1024*1024,3);
    std::vector<double> values(pixels*selected.size());std::vector<int> ordered;
    std::size_t selected_bytes=0;
    for(std::size_t c=0;c<selected.size();++c) {
        ordered.push_back(int(c));
        if(std::find(selected.begin(),selected.begin()+c,selected[c])==selected.begin()+c)selected_bytes+=spec.channelformat(selected[c]).size();
        for(std::size_t p=0;p<pixels;++p)values[p*selected.size()+c]=native_number(original.data()+p*stride+offsets[selected[c]],spec.channelformat(selected[c]));
    }
    std::vector<float> expected(pixels*4),actual(pixels*4);
    auto stats=map_data_double(values.data(),expected.data(),pixels,selected.size(),ordered,mapping);
    require(image.image.get_pixels(image.image.roi(),OIIO::TypeDesc::FLOAT,actual.data()),image.image.geterror());
    require(expected==actual,"data mapper lost native plane order/precision");
    require(image.statistics.invalid_pixels==stats.invalid_pixels && image.statistics.clipped_components==stats.clipped_components,"block statistics differ from full-image reference");
    require(image.decoded_bytes==pixels*selected_bytes,"native source includes unrelated channels");
    const auto rows=std::min(spec.height,std::max(32,spec.tile_height));
    require(image.native_buffer_bytes<=std::size_t(rows)*spec.width*selected_bytes,"data mapper retains a whole native image instead of a bounded row block");
    if(stats.invalid_pixels) {
        mapping.invalid=InvalidData::error;bool refused=false;
        try{read_data_image(*input,spec,0,selected,mapping,1024*1024,3);}
        catch(const InvalidDataValues& error){refused=true;require(error.statistics.invalid_pixels==stats.invalid_pixels && error.statistics.clipped_components==stats.clipped_components,"error policy lost statistics across blocks");}
        require(refused,"invalid samples accepted by error policy");mapping.invalid=InvalidData::magenta;
    }
    }
    require(input->close(),"close input");
}
static int run(const std::vector<fs::path>& args) {
    try {
        if(args.size()==8 && args[1]=="--fixture") {
            require(!fs::exists(args[2]),"fixture already exists");
            fixture(args[2],std::stoi(args[3].string()),std::stoi(args[4].string()),std::stoi(args[5].string()),std::stoi(args[6].string()),args[7].string());return 0;
        }
        auto root=fs::temp_directory_path()/("thumbgen-selected-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root),"owned fixture directory exists");
        struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,ec);}}cleanup{root};
        for(const auto& compression:{"zip","piz","none"})for(int tile:{0,8}) {
            auto path=root/(std::string(compression)+std::to_string(tile)+".exr");fixture(path,17,11,32,tile,compression);compare(path);
        }
        for(const auto& compression:{"zip","piz","none"})for(int tile:{0,8,64}) {
            auto path=root/(std::string(compression)+"-blocks-"+std::to_string(tile)+".exr");
            fixture(path,129,137,32,tile,compression);compare(path);
        }
        for(int tile:{0,64}) {
            auto path=root/("invalid-"+std::to_string(tile)+".exr");
            fixture(path,129,137,32,tile,"zip",true);compare(path,true);
        }
        auto path=root/"bounded.exr";fixture(path,256,256,64,0,"zip");
        thumbgen::Job job;job.id="compact-colour";job.input=path.u8string();job.options.max_decode_mib=1;
        job.outputs.push_back({(root/"bounded.png").u8string(),"png",32,32,90});thumbgen::Engine engine;
        auto result=engine.convert(job);require(result["ok"].get<bool>() && result["decoded_mib"]==1,result.dump());
        require(result["selection"]["chosen"]["group"]=="beauty",result.dump());
        std::cout<<"compact mixed native/FLOAT channel ranges, permutations, duplicates, tile edges and Beauty/root alpha passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int count,wchar_t** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#else
int main(int count,char** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#endif
