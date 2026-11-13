#include <thumbgen/core.h>
#include <OpenImageIO/imagebufalgo.h>
#include "resize_srgb.h"
#include "integer_storage.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
namespace iba=OIIO::ImageBufAlgo;
using namespace thumbgen;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static void expanding_storage(){
    for(unsigned bytes:{1u,2u})for(int channels:{3,4})for(bool gamma:{false,true}) {
        detail::ColourPlan plan;plan.route=detail::ColourPlan::Route::integer_linear;plan.sample_bytes=bytes;plan.input=*skcms_sRGB_profile();
        if(gamma){const skcms_TransferFunction transfer{2.2f,1,0,0,0,0,0};skcms_SetTransferFunction(&plan.input,&transfer);}
        for(std::size_t count:{1u,3u,7u,17u,8191u,8192u,8193u,65535u,65536u,65537u,131079u,1048579u}) {
            std::vector<std::uint16_t> native16(bytes==2?count*channels:0);std::vector<unsigned char> native8(bytes==1?count*channels:0);
            for(std::size_t i=0;i<count*channels;++i){auto value=std::uint32_t(i*1664525u+1013904223u);if(bytes==2)native16[i]=std::uint16_t(value);else native8[i]=static_cast<unsigned char>(value);}
            // Include zero alpha and its accompanying nonzero straight RGB.
            if(channels==4)for(std::size_t i=3;i<count*channels;i+=28){if(bytes==2)native16[i]=0;else native8[i]=0;}
            const void* native=bytes==2?static_cast<const void*>(native16.data()):static_cast<const void*>(native8.data());
            std::vector<float> expected(count*channels),storage(count*channels+16,.123f);
            plan.transform(native,expected.data(),count,channels);std::memcpy(storage.data(),native,count*channels*bytes);
            detail::transform_native_in_place(plan,storage.data(),count,channels);
            require(std::memcmp(storage.data(),expected.data(),count*channels*sizeof(float))==0,"bounded expansion changed float bits at block/SIMD boundary");
            for(std::size_t i=count*channels;i<storage.size();++i)require(storage[i]==.123f,"bounded expansion wrote beyond capacity");
            std::memcpy(storage.data(),native,count*channels*bytes);
            detail::transform_native_in_place<8192>(plan,storage.data(),count,channels);
            require(std::memcmp(storage.data(),expected.data(),count*channels*sizeof(float))==0,"small-block expansion changed float bits");
            for(std::size_t i=count*channels;i<storage.size();++i)require(storage[i]==.123f,"small-block expansion wrote beyond capacity");
        }
    }
}
static void ownership(){
    OIIO::ImageSpec spec(32,16,4,OIIO::TypeDesc::FLOAT);spec.alpha_channel=3;
    std::vector<float> raw(32*16*4,.25f);
    OIIO::ImageBuf borrowed(spec,raw.data()),working;
    require(iba::channels(working,borrowed,4,std::vector<int>{0,1,2,3}),working.geterror());
    require(working.storage()==OIIO::ImageBuf::LOCALBUFFER && working.localpixels()!=raw.data(),"channels retained borrowed pixels");
    borrowed.clear();std::fill(raw.begin(),raw.end(),.75f);std::vector<float>().swap(raw);
    OIIO::ImageBuf triangle,copy;
    require(iba::resize(triangle,working,{{"filtername","triangle"}},OIIO::ROI(0,8,0,4,0,1,0,4),1),triangle.geterror());
    require(copy.copy(working),copy.geterror());auto linear=detail::resize_linear(working,8,4);
    for(const auto* preview:{&triangle,&copy,&linear})require(preview->storage()==OIIO::ImageBuf::LOCALBUFFER && preview->localpixels()!=working.localpixels(),"preview aliases working pixels");
    working.clear();std::vector<float> overwrite(32*16*4,.9f);
    for(const auto* preview:{&triangle,&copy,&linear}){float pixel[4];preview->getpixel(0,0,pixel);for(float value:pixel)require(std::abs(value-.25f)<.00001f,"released source changed preview");}
}
int main(){try{
    expanding_storage();ownership();auto root=fs::temp_directory_path()/("thumbgen-lifetime-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    OIIO::ImageSpec spec(32,16,3,OIIO::TypeDesc::FLOAT);OIIO::ImageBuf source(spec);require(iba::fill(source,std::vector<float>{.2f,.3f,.4f}),source.geterror());require(source.write((root/"source.exr").u8string()),source.geterror());
    std::vector<std::string> phases;Engine engine([&](const std::string& phase,const Job&){phases.push_back(phase);});
    Job job;job.id="lifetime";job.input=(root/"source.exr").u8string();job.options.threads=1;job.options.input_space="linear";job.options.ocio_config="ocio://default";
    for(int width:{16,8}){Output out;out.path=(root/(std::to_string(width)+".png")).u8string();out.format="png";out.width=width;out.height=width;out.existing="replace";job.outputs.push_back(out);}
    auto converted=engine.convert(job);require(converted["ok"].get<bool>(),converted.dump());
    auto count=[&](const char* phase){return std::count(phases.begin(),phases.end(),phase);};
    require(count("memory_reference_source_released")+count("memory_reference_source_borrowed")==1 && count("memory_last_preview_source_released")==1,"missing lifetime checkpoints");
    auto released=std::find(phases.begin(),phases.end(),"memory_last_preview_source_released");require(std::count(phases.begin(),released,"encoded")==1 && std::count(released,phases.end(),"encoded")==1,"source released before first size or after last encode");
    phases.clear();job.outputs[1].existing="skip";converted=engine.convert(job);require(converted["ok"].get<bool>() && converted["outputs"][1]["status"]=="skipped",converted.dump());require(count("memory_last_preview_source_released")==0,"skipped last output released source early");
    phases.clear();job.outputs[0].existing="error";job.outputs[1].existing="replace";converted=engine.convert(job);require(converted["status"]=="partial" && converted["outputs"][1]["status"]=="success",converted.dump());require(count("memory_last_preview_source_released")==1,"partial output did not preserve last independent source");
    OIIO::ImageSpec grey_spec(32,16,1,OIIO::TypeDesc::FLOAT);grey_spec.channelnames={"Y"};OIIO::ImageBuf grey(grey_spec);require(iba::fill(grey,std::vector<float>{.2f}),grey.geterror());require(grey.write((root/"grey.exr").u8string()),grey.geterror());
    job.input=(root/"grey.exr").u8string();for(auto& out:job.outputs)out.existing="replace";
    phases.clear();converted=engine.convert(job);require(converted["ok"].get<bool>(),converted.dump());require(count("memory_reference_source_released")==1 && count("memory_reference_source_borrowed")==0,"grey expansion must materialize independent RGB");
    // DataImage transfers its ImageBuf into working; the moved-from handle
    // cannot be queried or cleared on the locked OIIO version.
    auto normal_spec=spec;normal_spec.channelnames={"N.x","N.y","N.z"};OIIO::ImageBuf normals(normal_spec);require(iba::fill(normals,std::vector<float>{.2f,.3f,.4f}),normals.geterror());require(normals.write((root/"normal.exr").u8string()),normals.geterror());
    job.input=(root/"normal.exr").u8string();job.options.input_space="auto";job.options.ocio_config.clear();job.options.channels="N";job.options.visualization="normal";for(auto& out:job.outputs)out.existing="replace";
    phases.clear();converted=engine.convert(job);require(converted["ok"].get<bool>(),converted.dump());require(count("memory_last_preview_source_released")==1,"data preview did not end source lifetime");
    OIIO::ImageSpec id_spec(32,16,1,OIIO::TypeDesc::UINT);id_spec.channelnames={"object_id"};OIIO::ImageBuf ids(id_spec);require(iba::fill(ids,std::vector<float>{2}),ids.geterror());require(ids.write((root/"ids.exr").u8string()),ids.geterror());
    job.input=(root/"ids.exr").u8string();job.options.channels="object_id";job.options.visualization="id";
    phases.clear();converted=engine.convert(job);require(converted["ok"].get<bool>(),converted.dump());require(count("memory_last_preview_source_released")==1,"categorical preview did not end native source lifetime");
    for(const auto& entry:fs::directory_iterator(root))fs::remove(entry.path());fs::remove(root);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
