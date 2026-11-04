#include "sampled_id.h"
#include "files.h"
#include <OpenImageIO/half.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
using namespace thumbgen::detail;
static void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
static void fixture(const fs::path& path,int orientation,int window,const std::string& compression,bool tiled) {
    OIIO::ImageSpec spec(1031,519,3,OIIO::TypeDesc::FLOAT);spec.channelnames={"unused.R","object_id","unused.A"};
    spec.channelformats={OIIO::TypeDesc::HALF,OIIO::TypeDesc::UINT32,OIIO::TypeDesc::FLOAT};
    spec.x=-7;spec.y=9;spec.full_x=-13;spec.full_y=2;spec.full_width=1049;spec.full_height=537;
    if(window==1){spec.full_x=spec.x+11;spec.full_y=spec.y+17;spec.full_width=991;spec.full_height=481;}
    if(window==2){spec.full_x=0;spec.full_y=0;spec.full_width=30001;spec.full_height=29001;}
    spec.attribute("Orientation",orientation);spec.attribute("compression",compression);spec.attribute("DateTime","2000:01:01 00:00:00");
    if(tiled){spec.tile_width=spec.tile_height=64;spec.tile_depth=1;}
    std::vector<unsigned char> data(std::size_t(spec.width)*spec.height*10);
    const std::uint32_t labels[]={0,1,42,16777216,16777217,4294967295U};
    for(int y=0;y<spec.height;++y)for(int x=0;x<spec.width;++x) {
        auto* pixel=data.data()+(std::size_t(y)*spec.width+x)*10;half a(0.5f);float b=0.25f;auto id=labels[(x/17+y/13)%6];
        std::memcpy(pixel,&a,2);std::memcpy(pixel+2,&id,4);std::memcpy(pixel+6,&b,4);
    }
    auto writer=OIIO::ImageOutput::create(path.u8string());require(bool(writer),OIIO::geterror());require(writer->open(path.u8string(),spec),writer->geterror());
    require(writer->write_image(OIIO::TypeDesc::UNKNOWN,data.data()),writer->geterror());require(writer->close(),writer->geterror());
}
static void compare(const fs::path& path) {
    auto input=OIIO::ImageInput::open(path.u8string());require(bool(input),OIIO::geterror());auto spec=input->spec();
    auto channel=std::find(spec.channelnames.begin(),spec.channelnames.end(),"object_id");require(channel!=spec.channelnames.end(),"ID channel renamed");
    std::vector<int> channels{int(channel-spec.channelnames.begin())};auto mapping=data_mapping("id",{},"error");
    std::vector<std::pair<int,int>> sizes{{1,1},{2,1},{3,2}};
    auto plan=sampled_id_plan(spec,input->format_name(),channels,mapping,sizes);require(bool(plan),"sparse plan unexpectedly declined");
    auto full=read_data_image(*input,spec,0,channels,mapping,64*1024*1024,2);
    auto sampled=read_sampled_id(*input,spec,0,*plan,mapping,1024*1024);require(input->close(),"cannot close fixture");
    require(sampled.decoded_bytes==plan->peak_bytes && sampled.decoded_bytes<full.decoded_bytes && sampled.statistics.invalid_pixels==0,"sampled source budget/statistics mismatch");
    for(auto size:sizes) {
        auto a=categorical_preview(full,size.first,size.second,1024*1024,2),b=categorical_preview(sampled,size.first,size.second,1024*1024,2);
        std::vector<float> x(std::size_t(size.first)*size.second*4),y(x.size());
        require(a.get_pixels(a.roi(),OIIO::TypeDesc::FLOAT,x.data()) && b.get_pixels(b.roi(),OIIO::TypeDesc::FLOAT,y.data()),"cannot read preview pixels");
        require(std::memcmp(x.data(),y.data(),x.size()*sizeof(float))==0,"sampled/full window/rotation pixels differ");
    }
    require(!sampled_id_plan(spec,"tiff",channels,mapping,sizes),"unmeasured format sampled");
    auto bad_orientation=spec;bad_orientation.attribute("Orientation",9);
    require(!sampled_id_plan(bad_orientation,"openexr",channels,mapping,sizes),"invalid orientation changed error staging");
    auto changed=spec;changed.channelformats[channels[0]]=OIIO::TypeDesc::FLOAT;
    require(!sampled_id_plan(changed,"openexr",channels,mapping,sizes),"floating IDs bypassed full invalid scan");
    require(!sampled_id_plan(spec,"openexr",channels,mapping,{{spec.full_width,spec.full_height}}),"dense or oversized preparation not declined");
}
int main() {
    try {
        auto root=fs::temp_directory_path()/("thumbgen-sampled-id-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));require(fs::create_directory(root),"cannot create owned test directory");
        struct Cleanup{fs::path root;~Cleanup(){std::error_code ec;if(root.parent_path()==fs::temp_directory_path())fs::remove_all(root,ec);}}cleanup{root};
        auto path=root/"mixed.exr";
        for(const std::string compression:{"zip","piz","zips"})for(int orientation=1;orientation<=8;++orientation)for(int window=0;window<3;++window) {
            fixture(path,orientation,window,compression,false);compare(path);
        }
        for(int orientation=1;orientation<=8;++orientation)for(int window=0;window<3;++window){fixture(path,orientation,window,"zip",true);compare(path);}
        std::cout<<"288 sampled/full native comparisons: mixed channels, 8 orientations, 3 windows, ZIP/PIZ/ZIPS/tiles passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
