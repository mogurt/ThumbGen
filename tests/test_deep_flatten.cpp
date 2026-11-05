#include "deep_flatten.h"
#include "deep_reference.h"
#include <OpenImageIO/imagebufalgo.h>
#include <nlohmann/json.hpp>
#include <thumbgen/core.h>
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
static void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
template<class Function> static void rejected(const char* code,Function function) {
    try{function();}catch(const DeepFailure& e){require(std::string(e.code)==code,"wrong Deep error code");return;}
    throw std::runtime_error("invalid Deep input accepted");
}
static DeepSample sample(float z,float back,std::array<float,3> colour,float alpha) {return {z,back,colour,{alpha,alpha,alpha}};}
static std::array<float,4> builtin(const std::vector<DeepSample>& samples,const fs::path& path={}) {
    OIIO::ImageSpec spec(1,1,6,OIIO::TypeDesc::FLOAT);spec.deep=true;spec.channelnames={"R","G","B","A","Z","Zback"};spec.alpha_channel=3;spec.z_channel=4;
    OIIO::ImageBuf source(spec);source.set_deep_samples(0,0,0,int(samples.size()));
    for(std::size_t s=0;s<samples.size();++s) {
        for(int c=0;c<3;++c)source.set_deep_value(0,0,0,c,int(s),samples[s].colour[c]);
        source.set_deep_value(0,0,0,3,int(s),samples[s].alpha[0]);source.set_deep_value(0,0,0,4,int(s),samples[s].z);source.set_deep_value(0,0,0,5,int(s),samples[s].zback);
    }
    if(!path.empty()){source.specmod().attribute("compression","zips");require(source.write(path.u8string()),source.geterror());}
    auto flat=OIIO::ImageBufAlgo::flatten(source);require(!flat.has_error(),flat.geterror());std::array<float,4> result{};flat.getpixel(0,0,result.data(),4);return result;
}
int main() {
    try {
        std::string text;for(const auto* part:deep_reference_json_parts)text+=part;
        auto reference=nlohmann::json::parse(text);DeepFlattener kernel;std::mt19937 random(94198);int comparisons=0,permutations=0;
        for(const auto& item:reference["cases"]) {
            std::vector<DeepSample> samples;
            for(const auto& s:item["samples"])samples.push_back({s["z"],s["zback"],s["colour"].get<std::array<float,3>>(),s["alpha"].get<std::array<float,3>>()});
            auto original=samples;auto result=kernel.flatten(samples.data(),samples.size());
            require(samples.empty() || std::memcmp(samples.data(),original.data(),samples.size()*sizeof(DeepSample))==0,"Deep source samples modified");
            for(int c=0;c<3;++c) {
                double colour=item["expected"]["colour"][c],alpha=item["expected"]["alpha"][c];
                require(std::abs(result.colour[c]-colour)<=2e-11*std::max(1.0,std::abs(colour)),item["id"].get<std::string>()+" Decimal colour mismatch");
                require(std::abs(result.alpha[c]-alpha)<=2e-11*std::abs(alpha)+std::numeric_limits<double>::denorm_min(),item["id"].get<std::string>()+" Decimal alpha mismatch");
            }
            if(item["id"]=="near-opaque-transmission")require(result.colour[1]>1e8,"rounded opacity discarded visible HDR transmission");
            if(item["id"]=="tiny-colour")require(result.colour[0]>0 && std::abs(result.colour[0]/item["expected"]["colour"][0].get<double>()-1)<1e-12,"tiny colour lost");
            for(int repeat=0;repeat<12;++repeat) {
                std::shuffle(samples.begin(),samples.end(),random);auto shuffled=kernel.flatten(samples.data(),samples.size());
                require(std::memcmp(&result,&shuffled,sizeof(result))==0,"Deep sample order changed output");++permutations;
            }
            ++comparisons;
        }
        auto root=fs::temp_directory_path()/("thumbgen-deep-kernel-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));require(fs::create_directory(root),"cannot reserve Deep test directory");
        struct Cleanup{fs::path root;~Cleanup(){std::error_code ec;if(root.parent_path()==fs::temp_directory_path())fs::remove_all(root,ec);}}cleanup{root};
        auto source_path=root/"unsorted.exr";
        auto reverse=builtin({sample(2,2,{0,0,.5f},.5f),sample(1,1,{.5f,0,0},.5f)},source_path);
        require(reverse[0]==.25f && reverse[2]==.5f,"OIIO unsorted-sample limitation changed; review probe");
        auto overlap=builtin({sample(0,1,{.5f,0,0},.5f),sample(0,1,{0,0,.5f},.5f)});
        require(overlap[0]==.5f && overlap[2]==.25f,"OIIO overlap limitation changed; review probe");
        auto bytes=[](const fs::path& path){std::ifstream file(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};auto original=bytes(source_path);
        thumbgen::Engine engine;auto inspection=engine.inspect(source_path.u8string());require(inspection["parts"][0]["deep"]==true,"generated file was not Deep");
        thumbgen::Job job;job.id="deep-still-explicit";job.input=source_path.u8string();job.outputs.push_back({(root/"preview.png").u8string(),"png",16,16,90});
        auto refused=engine.convert(job);require(refused["error"]["code"]=="deep_requires_flattening" && !fs::exists(job.outputs[0].path),"numeric kernel accidentally enabled implicit Deep conversion");
        require(original==bytes(source_path),"default Deep refusal changed source file");
        DeepFlattener sample_limit({2,1024*1024});rejected("deep_sample_limit_exceeded",[&]{sample_limit.flatten(nullptr,3);});require(!sample_limit.retained_workspace_bytes(),"sample rejection allocated workspace");
        DeepFlattener memory_limit({4096,1});std::vector<DeepSample> two{sample(1,1,{1,0,0},.5f),sample(2,2,{0,1,0},.5f)};
        rejected("deep_workspace_limit_exceeded",[&]{memory_limit.flatten(two.data(),two.size());});require(!memory_limit.retained_workspace_bytes(),"workspace rejection allocated buffers");
        rejected("deep_workspace_limit_exceeded",[]{DeepFlattener::workspace_bound(std::numeric_limits<std::size_t>::max());});
        auto invalid=two;invalid[0].alpha={1,1,1};invalid[1].colour[0]=std::numeric_limits<float>::quiet_NaN();rejected("deep_value_invalid",[&]{kernel.flatten(invalid.data(),invalid.size());});
        for(float value:{-1.0f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            invalid=two;invalid[1].alpha[0]=value;rejected("deep_value_invalid",[&]{kernel.flatten(invalid.data(),invalid.size());});
        }
        for(float value:{-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            invalid=two;invalid[1].z=value;rejected("deep_value_invalid",[&]{kernel.flatten(invalid.data(),invalid.size());});
        }
        auto retained=kernel.retained_workspace_bytes();kernel.flatten(nullptr,0);require(retained==kernel.retained_workspace_bytes() && retained<=8*1024*1024,"Deep workspace reuse is not bounded");
        std::cout<<comparisons<<" Decimal references and "<<permutations<<" exact permutation checks; OIIO raw-flatten limits and resource/invalid-value guards passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
