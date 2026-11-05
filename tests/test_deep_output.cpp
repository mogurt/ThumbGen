#include "deep_reader.h"
#include "deep_flatten.h"
#include "deep_reference.h"
#include "deep_background_reference.h"
#include <OpenImageIO/imagebuf.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
namespace fs=std::filesystem;
using namespace thumbgen::detail;
static void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
static DeepChannels channels(bool scalar=false){return {{"R","G","B"},scalar?std::array<std::string,3>{"A","A","A"}:std::array<std::string,3>{"AR","AG","AB"},"Z","Zback"};}
static void fixture(const fs::path& path,const std::vector<DeepSample>& samples,bool tiled=false,int width=1,int height=1,bool checker=false){
    OIIO::ImageSpec spec(width,height,9,OIIO::TypeDesc::FLOAT);spec.deep=true;spec.channelnames={"R","G","B","AR","AG","AB","Z","Zback","A"};spec.alpha_channel=8;spec.z_channel=6;spec.attribute("compression","zips");spec.x=spec.full_x=-3;spec.y=spec.full_y=4;
    OIIO::ImageBuf image(spec);if(tiled)image.set_write_tiles(2,2,1);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
    auto count=checker && (x+y)%2?0:samples.size();image.set_deep_samples(x-3,y+4,0,int(count));
    for(std::size_t s=0;s<count;++s){
        for(int c=0;c<3;++c){image.set_deep_value(x-3,y+4,0,c,int(s),samples[s].colour[c]);image.set_deep_value(x-3,y+4,0,c+3,int(s),samples[s].alpha[c]);}
        image.set_deep_value(x-3,y+4,0,6,int(s),samples[s].z);image.set_deep_value(x-3,y+4,0,7,int(s),samples[s].zback);image.set_deep_value(x-3,y+4,0,8,int(s),samples[s].alpha[0]);
    }
    }
    require(image.write(path.u8string()),image.geterror());
}
template<class Function> static void rejected(const char* code,Function function){
    try{function();}catch(const DeepFailure& error){require(std::string(error.code)==code,std::string("wrong output error: ")+error.code);return;}
    throw std::runtime_error("unsupported Deep output accepted");
}
static std::string bytes(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
int main(){
    try{
        auto root=fs::temp_directory_path()/("thumbgen-deep-output-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));require(fs::create_directory(root),"cannot reserve output fixture directory");
        struct Cleanup{fs::path path;~Cleanup(){std::error_code error;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,error);}}cleanup{root};
        std::string original_json,background_json;for(const auto* part:deep_reference_json_parts)original_json+=part;for(const auto* part:deep_background_reference_json_parts)background_json+=part;
        auto source=nlohmann::json::parse(original_json),reference=nlohmann::json::parse(background_json);require(source["cases"].size()==reference["cases"].size(),"background cases missing");
        auto path=root/"source.exr";int comparisons=0;
        for(std::size_t i=0;i<source["cases"].size();++i){
            const auto& item=source["cases"][i];const auto& expected=reference["cases"][i];require(item["id"]==expected["id"],"reference cases misaligned");
            std::vector<DeepSample> samples;for(const auto& value:item["samples"])samples.push_back({value["z"],value["zback"],value["colour"].get<std::array<float,3>>(),value["alpha"].get<std::array<float,3>>()});
            fixture(path,samples);const auto original=bytes(path);auto six=read_deep_image(path,0,channels());require(six.components==6 && six.bytes==24,"default six-channel contract changed");
            for(std::size_t b=0;b<reference["backgrounds"].size();++b){
                DeepReadOutput output{DeepOutputMode::opaque_rgba,reference["backgrounds"][b].get<std::array<double,3>>()};
                auto image=read_deep_image(path,0,channels(),{}, {},output);require(image.components==4 && image.bytes==16 && image.pixels[3]==1,"opaque RGBA layout/alpha differs");
                for(int c=0;c<3;++c){float wanted=expected["expected"][b][c];require(std::abs(image.pixels[c]-wanted)<=2e-6f*std::max(1.0f,std::abs(wanted)),item["id"].get<std::string>()+" background Decimal reference differs");}
                if(b==0)for(int c=0;c<3;++c)require(image.pixels[c]==six.pixels[c],"black background changed associated RGB");
                if(item["id"]=="near-opaque-transmission" && b==2)require(image.pixels[0]>1e8f,"rounded alpha discarded HDR background transmission");
                ++comparisons;
            }
            // Policy depends on selected metadata even when the samples happen
            // to have equal alphas or are empty; sequences cannot switch policy.
            rejected("deep_transparency_unsupported",[&]{read_deep_image(path,0,channels(),{}, {},{DeepOutputMode::associated_rgba,{}});});
            require(bytes(path)==original,"background composition changed input");
        }
        fixture(path,{{2,2,{0,0,.5f},{.5f,.5f,.5f}},{1,1,{.5f,0,0},{.5f,.5f,.5f}}});
        auto rgba=read_deep_image(path,0,channels(true),{}, {},{DeepOutputMode::associated_rgba,{}});
        require(rgba.components==4 && rgba.pixels[0]==.5f && rgba.pixels[2]==.25f && rgba.pixels[3]==.75f,"shared-alpha associated RGBA differs");
        auto peak=rgba.peak_accounted_bytes;DeepReadLimits limited;limited.bytes=peak;read_deep_image(path,0,channels(true),limited,{}, {DeepOutputMode::associated_rgba,{}});
        limited.bytes=peak-1;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,channels(true),limited,{}, {DeepOutputMode::associated_rgba,{}});});
        for(bool tiled:{false,true}){
            fixture(path,{{2,2,{0,0,.5f},{.5f,.5f,.5f}},{1,1,{.5f,0,0},{.5f,.5f,.5f}}},tiled,3,3,true);
            auto transparent=read_deep_image(path,0,channels(true),{}, {},{DeepOutputMode::associated_rgba,{}});
            auto white=read_deep_image(path,0,channels(),{}, {},{DeepOutputMode::opaque_rgba,{1,1,1}});
            require(white.x==-3 && white.y==4 && white.width==3 && white.height==3 && white.bytes==144 && white.chunks==(tiled?4:3),"RGBA grid/edge geometry differs");
            for(int y=0;y<3;++y)for(int x=0;x<3;++x){const bool empty=(x+y)%2!=0;const auto offset=(y*3+x)*4;
                const float expected_rgba[]={empty?0.f:.5f,0.f,empty?0.f:.25f,empty?0.f:.75f};
                const float expected_white[]={empty?1.f:.75f,empty?1.f:.25f,empty?1.f:.5f,1.f};
                for(int c=0;c<4;++c)require(transparent.pixels[offset+c]==expected_rgba[c] && white.pixels[offset+c]==expected_white[c],"RGBA scanline/tile/checker layout differs");
            }
        }
        fixture(path,{});auto empty=read_deep_image(path,0,channels(true),{}, {},{DeepOutputMode::associated_rgba,{}});for(int c=0;c<4;++c)require(empty.pixels[c]==0,"empty transparent RGBA differs");
        fixture(path,{{1,1,{2,0,0},{0,0,0}}});auto additive=read_deep_image(path,0,channels(true),{}, {},{DeepOutputMode::associated_rgba,{}});
        require(additive.pixels[0]==2 && additive.pixels[3]==0,"associated zero-alpha emission lost");
        fixture(path,{{1,1,{std::numeric_limits<float>::max(),0,0},{0,0,0}}});
        rejected("deep_value_invalid",[&]{read_deep_image(path,0,channels(),{}, {},{DeepOutputMode::opaque_rgba,{double(std::numeric_limits<float>::max()),0,0}});});
        for(auto output:std::vector<DeepReadOutput>{{DeepOutputMode::components,{1,0,0}},{DeepOutputMode::opaque_rgba,{std::numeric_limits<double>::quiet_NaN(),0,0}},{static_cast<DeepOutputMode>(99),{}}}){
            try{read_deep_image(path,0,channels(),{}, {},output);throw std::runtime_error("invalid output policy accepted");}catch(const std::invalid_argument&){}
        }
        std::cout<<comparisons<<" Decimal100 opaque background comparisons; stable scalar-alpha RGBA, HDR transmission, budget and emission guards passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
