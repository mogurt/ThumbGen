#include "deep_flatten.h"
#include "deep_reader.h"
#include "deep_infinite_reference.h"
#include <OpenImageIO/imagebuf.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
using namespace thumbgen::detail;
namespace fs=std::filesystem;
static void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
static float depth(const nlohmann::json& value) {
    if(value.is_string()){require(value=="positive_infinity","unknown depth encoding");return std::numeric_limits<float>::infinity();}
    return value.get<float>();
}
static void fixture(const fs::path& path,const std::vector<DeepSample>& samples) {
    OIIO::ImageSpec spec(1,1,8,OIIO::TypeDesc::FLOAT);spec.deep=true;spec.channelnames={"R","G","B","AR","AG","AB","Z","Zback"};spec.z_channel=6;
    spec.attribute("compression","zips");OIIO::ImageBuf image(spec);image.set_deep_samples(0,0,0,int(samples.size()));
    for(std::size_t s=0;s<samples.size();++s) {
        for(int c=0;c<3;++c){image.set_deep_value(0,0,0,c,int(s),samples[s].colour[c]);image.set_deep_value(0,0,0,c+3,int(s),samples[s].alpha[c]);}
        image.set_deep_value(0,0,0,6,int(s),samples[s].z);image.set_deep_value(0,0,0,7,int(s),samples[s].zback);
    }
    require(image.write(path.u8string()),image.geterror());
}
template<class F> static void rejected(const char* code,F function) {
    try{function();}catch(const DeepFailure& failure){require(std::string(failure.code)==code,std::string("wrong depth failure: ")+failure.code);return;}
    throw std::runtime_error("unsupported infinite depth accepted");
}
static std::string bytes(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
int main() {
    try {
        std::string text;for(const auto* part:deep_infinite_reference_json_parts)text+=part;
        const auto reference=nlohmann::json::parse(text);DeepFlattener kernel;std::mt19937 random(94198);
        auto root=fs::temp_directory_path()/("thumbgen-deep-infinite-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));require(fs::create_directory(root),"cannot reserve depth test directory");
        struct Cleanup{fs::path path;~Cleanup(){std::error_code error;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,error);}}cleanup{root};
        const DeepChannels channels{{"R","G","B"},{"AR","AG","AB"},"Z","Zback"};auto path=root/"input.exr";
        int cases=0,permutations=0,backgrounds=0;
        for(const auto& item:reference["cases"]) {
            std::vector<DeepSample> samples;for(const auto& s:item["samples"])samples.push_back({depth(s["z"]),depth(s["zback"]),s["colour"].get<std::array<float,3>>(),s["alpha"].get<std::array<float,3>>()});
            auto original=samples;auto result=kernel.flatten(samples.data(),samples.size());
            require(std::memcmp(samples.data(),original.data(),samples.size()*sizeof(DeepSample))==0,"infinite samples changed");
            for(int c=0;c<3;++c) {
                double colour=item["expected"]["colour"][c],alpha=item["expected"]["alpha"][c],through=item["transmission"][c];
                require(std::abs(result.colour[c]-colour)<=2e-11*std::max(1.,std::abs(colour)),item["id"].get<std::string>()+" infinite colour reference differs");
                require(std::abs(result.alpha[c]-alpha)<=2e-11*std::abs(alpha)+std::numeric_limits<double>::denorm_min(),"infinite alpha reference differs");
                require(std::abs(result.transmission[c]-through)<=2e-11*std::abs(through)+std::numeric_limits<double>::denorm_min(),"infinite transmission reference differs");
            }
            for(int repeat=0;repeat<12;++repeat){std::shuffle(samples.begin(),samples.end(),random);auto changed=kernel.flatten(samples.data(),samples.size());require(std::memcmp(&result,&changed,sizeof(result))==0,"infinite sample permutation changed output");++permutations;}
            fixture(path,original);const auto input=bytes(path);auto image=read_deep_image(path,0,channels);
            require(image.samples==samples.size(),"infinite reader sample count differs");
            for(int c=0;c<3;++c){float expected=item["expected"]["colour"][c];require(std::abs(image.pixels[c]-expected)<=2e-6f*std::max(1.f,std::abs(expected)),"infinite file reference differs");}
            for(std::size_t b=0;b<reference["backgrounds"].size();++b) {
                DeepReadOutput output{DeepOutputMode::opaque_rgba,reference["backgrounds"][b].get<std::array<double,3>>()};
                auto rgba=read_deep_image(path,0,channels,{}, {},output);require(rgba.components==4 && rgba.pixels[3]==1,"infinite opaque alpha differs");
                for(int c=0;c<3;++c){float expected=item["backgrounds"][b][c];require(std::abs(rgba.pixels[c]-expected)<=2e-6f*std::max(1.f,std::abs(expected)),"infinite background reference differs");}
                ++backgrounds;
            }
            if(item["id"]=="tiny-transmission-infinite-hdr")require(image.pixels[1]>1e8f,"infinite HDR emission lost through rounded alpha");
            require(bytes(path)==input,"infinite reader changed source");++cases;
        }
        const auto inf=std::numeric_limits<float>::infinity();
        for(float alpha:{0.f,.5f,1.f})for(bool hidden:{false,true}) {
            std::vector<DeepSample> invalid{{1,inf,{.5f,0,0},{alpha,alpha,alpha}}};
            if(hidden)invalid.insert(invalid.begin(),{0,0,{0,1,0},{1,1,1}});
            rejected("deep_depth_unsupported",[&]{kernel.flatten(invalid.data(),invalid.size());});fixture(path,invalid);
            rejected("deep_depth_unsupported",[&]{read_deep_image(path,0,channels);});
        }
        for(float value:{-inf,std::numeric_limits<float>::quiet_NaN()})for(int endpoint:{0,1}) {
            DeepSample invalid{1,1,{.5f,0,0},{.5f,.5f,.5f}};if(endpoint)invalid.zback=value;else invalid.z=value;
            rejected("deep_value_invalid",[&]{kernel.flatten(&invalid,1);});fixture(path,{invalid});rejected("deep_value_invalid",[&]{read_deep_image(path,0,channels);});
        }
        std::cout<<cases<<" infinite-point Decimal references, "<<permutations<<" exact permutations and "<<backgrounds<<" background comparisons; nonfinite volume/NaN/negative guards passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
