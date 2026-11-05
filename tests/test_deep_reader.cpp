#include "deep_reader.h"
#include "deep_flatten.h"
#include "files.h"
#include "deep_reference.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenEXR/openexr.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfDeepScanLineOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfPartType.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace thumbgen::detail;
namespace fs=std::filesystem;
static void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
static DeepChannels mapping(){return {{"Beauty.R","Beauty.G","Beauty.B"},{"Alpha.R","Alpha.G","Alpha.B"},"Depth.Z","Depth.Zback"};}
static std::string bytes(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
static void fixture(const fs::path& path,const std::vector<DeepSample>& samples,bool tiled=false,bool mixed=false,bool empty_first=false,const std::string& compression="zips",int rows=1,bool vary_rows=false){
    OIIO::ImageSpec spec(empty_first?2:1,rows,9,OIIO::TypeDesc::FLOAT);spec.deep=true;spec.x=-5;spec.y=7;spec.full_x=-5;spec.full_y=7;
    spec.channelnames={"Beauty.R","Beauty.G","Beauty.B","Alpha.R","Alpha.G","Alpha.B","Depth.Z","Depth.Zback","unused_id"};
    spec.z_channel=6;spec.alpha_channel=-1;spec.attribute("compression",compression);
    if(mixed){spec.channelformats.assign(9,OIIO::TypeDesc::HALF);spec.channelformats[6]=spec.channelformats[7]=OIIO::TypeDesc::FLOAT;spec.channelformats[8]=OIIO::TypeDesc::UINT32;}
    OIIO::ImageBuf image(spec);if(tiled)image.set_write_tiles(2,2,1);
    for(int y=0;y<spec.height;++y)for(int x=0;x<spec.width;++x){
        const int count=empty_first && x==0?0:vary_rows?(y%4==1?1:y%4==2?int(samples.size()):0):int(samples.size());image.set_deep_samples(spec.x+x,spec.y+y,0,count);
        for(int s=0;s<count;++s){for(int c=0;c<3;++c){image.set_deep_value(spec.x+x,spec.y+y,0,c,s,samples[s].colour[c]);image.set_deep_value(spec.x+x,spec.y+y,0,c+3,s,samples[s].alpha[c]);}
            image.set_deep_value(spec.x+x,spec.y+y,0,6,s,samples[s].z);image.set_deep_value(spec.x+x,spec.y+y,0,7,s,samples[s].zback);image.set_deep_value(spec.x+x,spec.y+y,0,8,s,std::uint32_t(17));}
    }
    require(image.write(path.u8string()),image.geterror());
}
template<class Function> static void rejected(const char* code,Function function){
    try{function();}catch(const DeepFailure& failure){require(std::string(failure.code)==code,std::string("wrong reader error: ")+failure.code);return;}
    throw std::runtime_error("Deep reader accepted rejected input");
}
static void native_fixture(const fs::path& path,OPENEXR_IMF_NAMESPACE::Compression compression){
    namespace Imf=OPENEXR_IMF_NAMESPACE;Imf::Header header(1,1);header.setType(Imf::DEEPSCANLINE);header.compression()=compression;
    const char* names[]={"R","G","B","A","Z","Zback"};float values[6][2]={{0,.5f},{0,0},{.5f,0},{.5f,.5f},{2,1},{2,1}};
    float* pointers[6]{};unsigned int count=2;Imf::DeepFrameBuffer frame;frame.insertSampleCountSlice(Imf::Slice(Imf::UINT,reinterpret_cast<char*>(&count),sizeof(count),sizeof(count)));
    for(int c=0;c<6;++c){header.channels().insert(names[c],Imf::Channel(Imf::FLOAT));pointers[c]=values[c];frame.insert(names[c],Imf::DeepSlice(Imf::FLOAT,reinterpret_cast<char*>(pointers+c),sizeof(float*),sizeof(float*),sizeof(float)));}
    Imf::DeepScanLineOutputFile output(path.u8string().c_str(),header,0);output.setFrameBuffer(frame);output.writePixels(1);
}
int main(){
    try{
        auto root=fs::temp_directory_path()/fs::u8path("thumbgen deep-reader 中文-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));require(fs::create_directory(root),"cannot reserve directory");
        struct Cleanup{fs::path path;~Cleanup(){std::error_code error;if(path.parent_path()==fs::temp_directory_path())fs::remove_all(path,error);}}cleanup{root};
        std::string text;for(const auto* chunk:deep_reference_json_parts)text+=chunk;auto references=nlohmann::json::parse(text);int comparisons=0;
        auto path=root/"references.exr";
        auto rle=root/"native-rle.exr";native_fixture(rle,OPENEXR_IMF_NAMESPACE::RLE_COMPRESSION);
        DeepChannels scalar_channels{{"R","G","B"},{"A","A","A"},"Z","Zback"};auto rle_image=read_deep_image(rle,0,scalar_channels);
        require(rle_image.pixels[0]==.5f && rle_image.pixels[2]==.25f && rle_image.pixels[3]==.75f,"native RLE/scalar alpha differs");
        auto zstd=root/"native-zstd.exr";native_fixture(zstd,OPENEXR_IMF_NAMESPACE::ZSTD_COMPRESSION);
        rejected("deep_layout_unsupported",[&]{read_deep_image(zstd,0,scalar_channels);});
        for(const auto& reference:references["cases"]){
            std::vector<DeepSample> samples;
            for(const auto& sample:reference["samples"])samples.push_back({sample["z"],sample["zback"],sample["colour"].get<std::array<float,3>>(),sample["alpha"].get<std::array<float,3>>()});
            fixture(path,samples);const auto original=bytes(path);auto image=read_deep_image(path,0,mapping());
            require(image.x==-5 && image.y==7 && image.width==1 && image.height==1 && image.samples==samples.size(),"Deep geometry/sample metadata differs");
            for(int c=0;c<6;++c){const float expected=reference["expected"][c<3?"colour":"alpha"][c%3].get<float>();
                require(std::abs(image.pixels[c]-expected)<=2e-6f*std::abs(expected)+std::numeric_limits<float>::denorm_min(),reference["id"].get<std::string>()+" decoded Decimal reference differs");}
            require(bytes(path)==original && image.peak_accounted_bytes>=image.bytes && image.peak_accounted_bytes<=512*1024*1024,"reader source/budget invariant");++comparisons;
        }
        const std::vector<DeepSample> points={{2,2,{0,.75f,.5f},{0,.75f,.5f}},{1,1,{.5f,.25f,0},{.5f,.25f,0}}};
        for(bool tiled:{false,true})for(bool mixed:{false,true})for(const std::string compression:{"none","zips"}){
            fixture(path,points,tiled,mixed,true,compression,3);auto image=read_deep_image(path,0,mapping());
            require(image.width==2 && image.height==3 && image.samples==6 && image.chunks==(tiled?2:3),"mixed fixture geometry differs");
            const float expected[]={.5f,.8125f,.5f,.5f,.8125f,.5f};
            for(int y=0;y<3;++y)for(int c=0;c<6;++c){require(image.pixels[y*12+c]==0,"empty Deep pixel is not black/transparent");require(image.pixels[y*12+6+c]==expected[c],"associated per-component alpha/half/UINT skip differs");}
            DeepReadLimits limits;limits.bytes=image.peak_accounted_bytes;auto exact=read_deep_image(path,0,mapping(),limits);require(exact.peak_accounted_bytes==limits.bytes,"exact combined budget differs");
            --limits.bytes;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,mapping(),limits);});
        }
        for(bool tiled:{false,true})for(bool mixed:{false,true})for(const std::string compression:{"none","zips"}){
            fixture(path,points,tiled,mixed,true,compression,9,true);auto original=bytes(path);auto varied=read_deep_image(path,0,mapping());
            require(varied.chunks==(tiled?5:9) && varied.samples==6,"changing chunk sample count differs");
            const float pair[]={.5f,.8125f,.5f,.5f,.8125f,.5f};
            const float single[]={0,.75f,.5f,0,.75f,.5f};
            for(int y=0;y<9;++y)for(int c=0;c<6;++c){
                require(varied.pixels[y*12+c]==0,"empty column retained prior chunk data");
                const float expected=y%4==1?single[c]:y%4==2?pair[c]:0;
                require(varied.pixels[y*12+6+c]==expected,"changing chunk retained stale counts or channel pointers");
            }
            require(bytes(path)==original,"changing-chunk reader changed source");
            DeepReadLimits exact;exact.bytes=varied.peak_accounted_bytes;require(read_deep_image(path,0,mapping(),exact).peak_accounted_bytes==exact.bytes,"changing-chunk budget differs");
            --exact.bytes;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,mapping(),exact);});
            int observed=0;try {
                read_deep_image(path,0,mapping(),{},[&](const char* phase){if(std::string(phase)=="counts" && ++observed==3)throw std::runtime_error("later chunk cancellation");});
                throw std::runtime_error("later cancellation ignored");
            }catch(const std::runtime_error& failure){require(std::string(failure.what())=="later chunk cancellation","later cancellation changed");}
            require(bytes(path)==original,"later cancellation changed source");
        }
        for(const std::string compression:{"none","zips"}){
            fixture(path,points,false,false,false,compression,1);auto single=read_deep_image(path,0,mapping());
            fixture(path,points,false,false,false,compression,32);auto many=read_deep_image(path,0,mapping());
            std::cout<<compression<<" allocator requests: one chunk="<<single.core_allocation_requests<<", 32 chunks="<<many.core_allocation_requests<<'\n';
            // The failure-safe legacy ZIP path still creates one codec
            // context per compressed chunk. Require reuse of the other Core
            // buffers without requiring that upstream codec cache to work.
            const auto codec_requests=compression=="zips"?many.chunks-single.chunks:0;
            require(many.core_allocation_requests<=single.core_allocation_requests+codec_requests+8,"Core buffer allocations scale with identical chunks");
        }
        fixture(path,points,false,false,true,"none",3);DeepReadLimits accumulated;accumulated.samples_per_image=4;int decoded_chunks=0;
        rejected("deep_sample_limit_exceeded",[&]{read_deep_image(path,0,mapping(),accumulated,[&](const char* phase){if(std::string(phase)=="decoded_chunk")++decoded_chunks;});});
        require(decoded_chunks==2,"image sample budget did not accumulate across chunks");
        fixture(path,points);auto peak=read_deep_image(path,0,mapping()).peak_accounted_bytes;std::size_t budget_refusals=0;
        for(std::size_t cap=1;cap<peak;++cap){DeepReadLimits bounded;bounded.bytes=cap;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,mapping(),bounded);});++budget_refusals;}
        DeepReadLimits limits;limits.samples_per_pixel=1;rejected("deep_sample_limit_exceeded",[&]{read_deep_image(path,0,mapping(),limits);});
        limits={};limits.samples_per_chunk=1;rejected("deep_sample_limit_exceeded",[&]{read_deep_image(path,0,mapping(),limits);});
        limits={};limits.samples_per_image=1;rejected("deep_sample_limit_exceeded",[&]{read_deep_image(path,0,mapping(),limits);});
        limits={};limits.bytes=1;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,mapping(),limits);});
        auto missing=mapping();missing.z="missing";rejected("deep_layout_unsupported",[&]{read_deep_image(path,0,missing);});
        auto opaque=mapping();opaque.alpha={};opaque.zback.clear();auto opaque_image=read_deep_image(path,0,opaque);
        require(opaque_image.pixels[0]==.5f && opaque_image.pixels[1]==.25f && opaque_image.pixels[2]==0 && opaque_image.pixels[3]==1,"explicit opaque/point defaults differ");
        auto scalar=mapping();scalar.colour={"Beauty.R","Beauty.R","Beauty.R"};scalar.alpha={"Alpha.R","Alpha.R","Alpha.R"};auto luminance=read_deep_image(path,0,scalar);
        require(luminance.pixels[0]==.5f && luminance.pixels[1]==.5f && luminance.pixels[2]==.5f,"duplicate selected channels differ");
        fixture(path,points,false,true);auto integers=mapping();integers.colour[0]="unused_id";rejected("deep_layout_unsupported",[&]{read_deep_image(path,0,integers);});
        auto invalid=points;invalid[1].alpha={1,1,1};invalid[0].colour[0]=std::numeric_limits<float>::quiet_NaN();fixture(path,invalid);
        rejected("deep_value_invalid",[&]{read_deep_image(path,0,mapping());});
        fixture(path,points);auto original=bytes(path);
        try{read_deep_image(path,0,mapping(),{},[](const char* phase){if(std::string(phase)=="counts")throw std::runtime_error("test cancellation");});throw std::runtime_error("cancellation ignored");}
        catch(const std::runtime_error& error){require(std::string(error.what())=="test cancellation","cancellation error changed");}
        require(bytes(path)==original,"cancelled reader changed source");
        try{read_deep_image(path,0,mapping(),{},[&](const char* phase){if(std::string(phase)=="counts"){std::ofstream output(path,std::ios::binary|std::ios::app);output<<'x';}});throw std::runtime_error("source change ignored");}
        catch(const FileFailure& error){require(error.code=="source_changed" && error.retryable,"source change classification differs");}
        fixture(path,points);original=bytes(path);fs::resize_file(path,original.size()-1);rejected("deep_decode_failed",[&]{read_deep_image(path,0,mapping());});
        fixture(path,points);exr_context_t oversized_context=nullptr;require(exr_start_read(&oversized_context,path.u8string().c_str(),nullptr)==EXR_ERR_SUCCESS,"cannot inspect oversized fixture");
        exr_chunk_info_t oversized_chunk{};require(exr_read_scanline_chunk_info(oversized_context,0,7,&oversized_chunk)==EXR_ERR_SUCCESS,"cannot locate oversized chunk");exr_finish(&oversized_context);
        {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(std::streamoff(oversized_chunk.sample_count_data_offset)-8);const char size[8]={0,0,0,1,0,0,0,0};file.write(size,8);}
        DeepReadLimits small;small.bytes=8*1024*1024;rejected("deep_decode_limit_exceeded",[&]{read_deep_image(path,0,mapping(),small);});
        fixture(path,points,false,false,true,"none");
        exr_context_t context=nullptr;require(exr_start_read(&context,path.u8string().c_str(),nullptr)==EXR_ERR_SUCCESS,"cannot inspect count guard fixture");
        exr_chunk_info_t chunk{};require(exr_read_scanline_chunk_info(context,0,7,&chunk)==EXR_ERR_SUCCESS,"cannot locate count table");exr_finish(&context);
        auto timestamp=fs::last_write_time(path);
        rejected("deep_decode_failed",[&]{read_deep_image(path,0,mapping(),{},[&](const char* phase){if(std::string(phase)=="counts"){
            std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(std::streamoff(chunk.sample_count_data_offset));const char changed[4]={1,0,0,0};file.write(changed,4);file.close();fs::last_write_time(path,timestamp);
        }});});
        fixture(path,points);std::vector<std::future<DeepImage>> readers;
        for(int n=0;n<4;++n)readers.push_back(std::async(std::launch::async,[&]{return read_deep_image(path,0,mapping());}));
        for(auto& reader:readers)require(reader.get().samples==2,"independent concurrent reader differs");
        std::cout<<comparisons<<" decoded Decimal references; "<<budget_refusals<<" consecutive below-peak budget refusals; mixed scan/tile/alpha/cancellation/source/count-guard and independent threads passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
