#include <OpenImageIO/imagebuf.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <limits>
namespace fs=std::filesystem;
static OIIO::ImageBuf fixture(const std::string& kind) {
    if(kind=="flat") {
        OIIO::ImageSpec spec(4,3,4,OIIO::TypeDesc::FLOAT);spec.alpha_channel=3;spec.attribute("oiio:ColorSpace","linear");
        OIIO::ImageBuf image(spec);float pixel[4]={.5f,0,.25f,.75f};
        for(int y=0;y<3;++y)for(int x=0;x<4;++x)image.setpixel(x,y,pixel);
        return image;
    }
    std::vector<std::string> names{"R","G","B","A","Z","Zback"};
    if(kind=="coloured" || kind=="changed_alpha")names={"R","G","B","AR","AG","AB","Z","Zback"};
    if(kind=="beauty")names={"R","G","B","A","beauty.R","beauty.G","beauty.B","Z","Zback"};
    if(kind=="missing_alpha")names={"R","G","B","Z","Zback"};
    if(kind=="no_zback" || kind=="infinite_no_zback")names.pop_back();
    if(kind=="depth_collision")names={"beauty.R","beauty.G","beauty.B","A","Z","Zback","beauty.Z","beauty.Zback"};
    if(kind=="ancestor_alpha")names={"view.beauty.R","view.beauty.G","view.beauty.B","view.A","A","Z","Zback"};
    if(kind=="ancestor_alpha_shift")names={"view.beauty.R","view.beauty.G","view.beauty.B","A","Z","Zback"};
    if(kind=="mixed_alpha")names={"R","G","B","AR","A","Z","Zback"};
    if(kind=="grey_alpha")names={"Y","A","AR","AG","AB","Z","Zback"};
    if(kind.rfind("opacity",0)==0)names={"A","Z","ZBack"};
    if(kind=="opacity_points" || kind=="opacity_ordered")names.pop_back();
    if(kind=="opacity_extra")names.push_back("mask");
    if(kind=="reordered")std::reverse(names.begin(),names.end());
    OIIO::ImageSpec spec(4,3,int(names.size()),OIIO::TypeDesc::FLOAT);spec.deep=true;spec.channelnames=names;
    if(kind=="opacity_half")spec.channelformats={OIIO::TypeDesc::HALF,OIIO::TypeDesc::FLOAT,OIIO::TypeDesc::FLOAT};
    spec.x=spec.full_x=-3;spec.y=spec.full_y=4;
    if(kind=="window" || kind=="opacity_window"){spec.full_x=-4;spec.full_y=3;spec.full_width=6;spec.full_height=5;}
    spec.alpha_channel=-1;spec.z_channel=-1;
    for(int i=0;i<spec.nchannels;++i){if(names[i]=="A")spec.alpha_channel=i;if(names[i]=="Z")spec.z_channel=i;}
    spec.attribute("compression","zips");spec.attribute("oiio:ColorSpace",kind=="nonlinear"?"sRGB":"linear");
    OIIO::ImageBuf image(spec);if(kind=="tiled" || kind=="opacity_overlap")image.set_write_tiles(2,2,1);
    for(int y=4;y<7;++y)for(int x=-3;x<1;++x) {
        const int count=kind=="empty"?0:kind=="emission" || kind=="ancestor_alpha" || kind=="ancestor_alpha_shift" || kind=="mixed_alpha" || kind=="grey_alpha"?1:2;image.set_deep_samples(x,y,0,count);
        for(int s=0;s<count;++s)for(int c=0;c<spec.nchannels;++c) {
            const auto& name=names[c];float value=0;
            if(name=="R" || name=="beauty.R")value=kind=="emission"?2.f:s==1?.5f:0.f;
            if(name=="B" || name=="beauty.B")value=kind=="emission"?0.f:s==0?.5f:0.f;
            if(kind=="beauty" && (name=="R" || name=="G" || name=="B"))value=0;
            if(name=="A" || name=="AR" || name=="AG" || name=="AB")value=kind=="emission"?0.f:.5f;
            if(kind=="coloured" && name=="AG")value=.25f;
            if(kind=="coloured" && name=="AB")value=.75f;
            if(name=="Z" || name=="Zback")value=kind=="emission"?1.f:s==0?2.f:1.f;
            if(kind=="volumes" && name=="Zback")value+=1;
            if((kind=="infinite_point" || kind=="infinite_reversed_point" || kind=="infinite_no_zback") && s==0 &&
                (name=="Z" || (name=="Zback" && kind!="infinite_reversed_point")))value=std::numeric_limits<float>::infinity();
            if(kind=="semi_infinite_volume" && s==1 && name=="Zback")value=std::numeric_limits<float>::infinity();
            if(kind=="depth_collision" && (name=="beauty.Z" || name=="beauty.Zback"))value=s==0?1.f:2.f;
            if(kind=="ancestor_alpha" || kind=="ancestor_alpha_shift") {
                if(name=="view.beauty.R")value=.125f;if(name=="view.beauty.G")value=.0625f;if(name=="view.beauty.B")value=.1875f;
                if(name=="view.A")value=.25f;if(name=="A")value=.75f;
            }
            if(kind=="mixed_alpha") {
                if(name=="R" || name=="AR")value=.25f;if(name=="G" || name=="B" || name=="A")value=.5f;
            }
            if(kind=="grey_alpha") {
                if(name=="Y")value=.25f;
                if(name=="AR")value=.125f;if(name=="AG")value=.25f;if(name=="AB")value=.75f;
            }
            if(kind.rfind("opacity",0)==0) {
                const int sample=kind=="opacity_ordered"?1-s:s;
                if(name=="A")value=kind=="opacity_invalid" && s==0?std::numeric_limits<float>::quiet_NaN():sample==0?.25f:.5f;
                if(kind=="opacity_window" && name=="A" && y==4) {
                    if(x==-3)value=0;
                    if(x==-2)value=1;
                }
                if(name=="Z")value=sample==0?2.f:1.f;
                if(name=="ZBack")value=(s==0?2.f:1.f)+(kind=="opacity_overlap"?2.f:0.f);
            }
            image.set_deep_value(x,y,0,c,s,value);
        }
    }
    return image;
}
static void multipart(const fs::path& path,const std::string& first) {
    auto other=fixture(first),opacity=fixture("opacity_points");
    opacity.specmod().attribute("oiio:subimagename","coverage");
    other.specmod().attribute("oiio:subimagename","image");
    OIIO::ImageSpec parts[]{other.spec(),opacity.spec()};
    auto out=OIIO::ImageOutput::create(path.u8string());
    if(!out)throw std::runtime_error(OIIO::geterror());
    if(!out->open(path.u8string(),2,parts) || !out->write_deep_image(*other.deepdata()) ||
       !out->open(path.u8string(),parts[1],OIIO::ImageOutput::AppendSubimage) ||
       !out->write_deep_image(*opacity.deepdata()) || !out->close())
        throw std::runtime_error(out->geterror());
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("expected new fixture directory");
        auto root=fs::absolute(fs::u8path(argv[1]));if(!fs::create_directory(root))throw std::runtime_error("fixture directory exists");
        nlohmann::json files=nlohmann::json::object();
        for(const char* kind:{"scalar","tiled","coloured","changed_alpha","beauty","missing_alpha","no_zback","reordered","empty","window","nonlinear","emission","volumes","flat","depth_collision","ancestor_alpha","ancestor_alpha_shift","mixed_alpha","grey_alpha","infinite_point","infinite_reversed_point","infinite_no_zback","semi_infinite_volume",
                              "opacity_points","opacity_ordered","opacity_overlap","opacity_half","opacity_window","opacity_invalid","opacity_extra"}) {
            auto path=root/(std::string(kind)+".exr");
            auto image=fixture(kind);if(!image.write(path.u8string()))throw std::runtime_error(image.geterror());
            files[kind]=path.u8string();
        }
        for(const char* first:{"scalar","opacity_points"}) {
            const auto kind=std::string("multipart_")+first;
            auto path=root/(kind+".exr");multipart(path,first);files[kind]=path.u8string();
        }
        std::cout<<files.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
