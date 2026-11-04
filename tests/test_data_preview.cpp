#include "data_preview.h"
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace thumbgen::detail;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class Function> static void rejects(Function fn) {
    try{fn();}catch(const std::invalid_argument&){return;}
    throw std::runtime_error("invalid data request was accepted");
}
static void rgb(const std::vector<float>& pixels,std::size_t p,float r,float g,float b) {
    require(std::abs(pixels[p*4]-r)<1e-7f && std::abs(pixels[p*4+1]-g)<1e-7f &&
        std::abs(pixels[p*4+2]-b)<1e-7f && pixels[p*4+3]==1,"mapped RGBA reference mismatch");
}
template<class T> static void native_roundtrip(const std::filesystem::path& path,OIIO::TypeDesc type,const std::vector<T>& values) {
    OIIO::ImageSpec spec(int(values.size()),1,1,type);spec.channelnames={"object_id"};
    auto writer=OIIO::ImageOutput::create(path.u8string());require(bool(writer),"ID fixture writer unavailable");
    require(writer->open(path.u8string(),spec),"ID fixture open");
    require(writer->write_image(type,values.data()),"ID fixture write");require(writer->close(),"ID fixture close");
    auto reader=OIIO::ImageInput::open(path.u8string());require(bool(reader),"ID fixture reader unavailable");
    require(reader->spec().width==int(values.size()) && reader->spec().height==1 && reader->spec().nchannels==1 &&
        reader->spec().format==type,"native integer fixture type/shape changed");
    std::vector<T> decoded(values.size());
    require(reader->read_image(0,0,0,1,type,decoded.data()),"native ID read failed");
    require(decoded==values,"native integer ID did not round-trip exactly");
    std::vector<float> floats(values.size());
    require(reader->read_image(0,0,0,1,OIIO::TypeDesc::FLOAT,floats.data()),"control FLOAT ID read failed");
    require(reader->close(),"ID reader close");
    // Adjacent IDs well above 2^24 collide whether FLOAT conversion is numeric
    // or normalized by the input plugin.
    require(floats[3]==floats[4],"control read unexpectedly preserved adjacent IDs through FLOAT");
    std::vector<float> before(values.size()*4),after(before.size());
    if constexpr(std::is_same_v<T,std::uint32_t>) {
        map_ids_u32(values.data(),before.data(),values.size(),1,0);
        map_ids_u32(decoded.data(),after.data(),decoded.size(),1,0);
    } else {
        map_ids_i32(values.data(),before.data(),values.size(),1,0);
        map_ids_i32(decoded.data(),after.data(),decoded.size(),1,0);
    }
    require(before==after,"native read changed ID palette");
}
int main() {
    try {
        for(const auto& mode:{"depth","vector"})rejects([&]{data_mapping(mode,{},"magenta");});
        for(const std::vector<double>& range:std::vector<std::vector<double>>{{1,1},{2,1},{0},{0,1,2},
            {0,std::numeric_limits<double>::infinity()},{-1e308,1e308},{0,std::numeric_limits<double>::quiet_NaN()}})
            rejects([&]{data_mapping("depth",range,"magenta");});
        rejects([]{data_mapping("id",{0,1},"black");});
        rejects([]{data_mapping("colour",{},"error");});
        rejects([]{data_mapping("unknown",{},"magenta");});
        rejects([]{data_mapping("normal",{},"unknown");});
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
        auto depth=data_mapping("depth",{-10,10},"magenta");
        std::vector<float> source={-20,-10,0,10,20,nan,inf,-inf},target(8*4);
        auto stats=map_data_float(source.data(),target.data(),8,1,{0},depth);
        require(stats.invalid_pixels==3 && stats.clipped_components==2,"depth invalid/clipped statistics");
        rgb(target,0,0,0,0);rgb(target,1,0,0,0);rgb(target,2,.5f,.5f,.5f);rgb(target,3,1,1,1);
        rgb(target,4,1,1,1);for(int p:{5,6,7})rgb(target,p,1,0,1);
        {
            auto mapping=data_mapping("depth",{0,1},"black");
            stats=map_data_float(&nan,target.data(),1,1,{0},mapping);
            require(stats.invalid_pixels==1,"black invalid count");rgb(target,0,0,0,0);
        }
        bool invalid_error=false;
        try{map_data_float(&nan,target.data(),1,1,{0},data_mapping("depth",{0,1},"error"));}
        catch(const InvalidDataValues& error){invalid_error=error.statistics.invalid_pixels==1;}
        require(invalid_error,"error policy must fail rather than silently render black");
        // The same value has the same mapping even when other frame values change.
        const float frame_a[]={5,0,10},frame_b[]={5,-1000,1000};float a[12],b[12];
        map_data_float(frame_a,a,3,1,{0},data_mapping("depth",{0,10},"magenta"));
        map_data_float(frame_b,b,3,1,{0},data_mapping("depth",{0,10},"magenta"));
        require(std::equal(a,a+4,b),"mapping depends on frame statistics");
        // Component order and channel stride are independent of file ordering.
        const float normals[]={1,42,-1,0, 0,42,nan,1};
        stats=map_data_float(normals,target.data(),2,4,{2,3,0},data_mapping("normal",{},"magenta"));
        rgb(target,0,0,.5f,1);rgb(target,1,1,0,1);require(stats.invalid_pixels==1,"normal invalid pixel");
        const float vectors[]={-2,99,2, 0,99,-4};
        stats=map_data_float(vectors,target.data(),2,3,{2,0},data_mapping("vector",{-2,2},"black"));
        rgb(target,0,1,0,.5f);rgb(target,1,0,.5f,.5f);require(stats.clipped_components==1,"vector clip count");
        map_data_float(normals,target.data(),1,4,{2,3,0},data_mapping("normal",{-2,2},"magenta"));
        rgb(target,0,.25f,.5f,.75f);
        // Full UINT32 identity survives beyond FLOAT's consecutive integer range.
        const std::uint32_t ids[]={0,1,42,16777216,16777217,4294967295u};
        target.resize(6*4);map_ids_u32(ids,target.data(),6,1,0);rgb(target,0,0,0,0);
        rgb(target,1,119/255.0f,178/255.0f,193/255.0f);
        rgb(target,2,213/255.0f,140/255.0f,248/255.0f);
        rgb(target,3,224/255.0f,142/255.0f,64/255.0f);
        rgb(target,4,87/255.0f,192/255.0f,129/255.0f);
        rgb(target,5,182/255.0f,146/255.0f,193/255.0f);
        require(!std::equal(target.begin()+12,target.begin()+16,target.begin()+16),"adjacent UINT32 IDs collapsed through FLOAT");
        const auto baseline=target;
        for(std::size_t p=0;p<6;++p)map_ids_u32(ids+p,target.data()+p*4,1,1,0);
        require(target==baseline,"ID palette changed with chunking");
        const std::int32_t signed_ids[]={-1};float signed_pixel[4];map_ids_i32(signed_ids,signed_pixel,1,1,0);
        require(std::equal(signed_pixel,signed_pixel+4,target.begin()+20),"signed ID bit identity");
        const std::uint32_t interleaved[]={123,0,123,1,123,42};float strided[12];
        map_ids_u32(interleaved,strided,3,2,1);
        require(std::equal(strided,strided+12,baseline.begin()),"native ID stride/component mapping");
        const float float_ids[]={0,1,-1,1.5f,nan,4294967296.0f};target.resize(24);
        stats=map_data_float(float_ids,target.data(),6,1,{0},data_mapping("id",{},"magenta"));
        require(stats.invalid_pixels==3 && stats.clipped_components==0,"FLOAT IDs must be integral and fit 32 bits");
        require(std::equal(target.begin()+8,target.begin()+12,signed_pixel),"FLOAT signed ID identity");
        rgb(target,3,1,0,1);rgb(target,4,1,0,1);rgb(target,5,1,0,1);
        rejects([&]{map_data_float(source.data(),target.data(),1,1,{0,0},depth);});
        rejects([&]{map_data_float(source.data(),target.data(),1,1,{-1},depth);});
        rejects([&]{map_data_float(source.data(),target.data(),1,1,{1},depth);});
        rejects([&]{map_data_float(nullptr,target.data(),1,1,{0},depth);});
        rejects([&]{map_ids_u32(ids,target.data(),std::numeric_limits<std::size_t>::max(),2,0);});
        auto root=std::filesystem::temp_directory_path()/("thumbgen-data-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(root),"ID fixture directory exists");
        struct Cleanup {
            std::filesystem::path root;
            ~Cleanup(){std::error_code ec;std::filesystem::remove(root/"ids.exr",ec);std::filesystem::remove(root/"ids.tif",ec);std::filesystem::remove(root,ec);}
        } cleanup{root};
        native_roundtrip<std::uint32_t>(root/"ids.exr",OIIO::TypeDesc::UINT32,{0,1,42,1073741824,1073741825,4294967295u});
        native_roundtrip<std::int32_t>(root/"ids.tif",OIIO::TypeDesc::INT32,{-1,std::numeric_limits<std::int32_t>::min(),0,1073741824,1073741825,std::numeric_limits<std::int32_t>::max()});
        std::cout<<"fixed ranges, signed vectors, invalid policies and full 32-bit ID mapping passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
