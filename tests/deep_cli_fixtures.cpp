#include <OpenImageIO/imagebuf.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>
namespace fs=std::filesystem;
static void fixture(const fs::path& path,const std::string& kind) {
    if(kind=="flat") {
        OIIO::ImageSpec spec(4,3,4,OIIO::TypeDesc::FLOAT);spec.alpha_channel=3;spec.attribute("oiio:ColorSpace","linear");
        OIIO::ImageBuf image(spec);float pixel[4]={.5f,0,.25f,.75f};
        for(int y=0;y<3;++y)for(int x=0;x<4;++x)image.setpixel(x,y,pixel);
        if(!image.write(path.u8string()))throw std::runtime_error(image.geterror());return;
    }
    std::vector<std::string> names{"R","G","B","A","Z","Zback"};
    if(kind=="coloured" || kind=="changed_alpha")names={"R","G","B","AR","AG","AB","Z","Zback"};
    if(kind=="beauty")names={"R","G","B","A","beauty.R","beauty.G","beauty.B","Z","Zback"};
    if(kind=="missing_alpha")names={"R","G","B","Z","Zback"};
    if(kind=="no_zback")names.pop_back();
    if(kind=="reordered")std::reverse(names.begin(),names.end());
    OIIO::ImageSpec spec(4,3,int(names.size()),OIIO::TypeDesc::FLOAT);spec.deep=true;spec.channelnames=names;
    spec.x=spec.full_x=-3;spec.y=spec.full_y=4;
    if(kind=="window"){spec.full_x=-4;spec.full_y=3;spec.full_width=6;spec.full_height=5;}
    spec.alpha_channel=-1;spec.z_channel=-1;
    for(int i=0;i<spec.nchannels;++i){if(names[i]=="A")spec.alpha_channel=i;if(names[i]=="Z")spec.z_channel=i;}
    spec.attribute("compression","zips");spec.attribute("oiio:ColorSpace",kind=="nonlinear"?"sRGB":"linear");
    OIIO::ImageBuf image(spec);if(kind=="tiled")image.set_write_tiles(2,2,1);
    for(int y=4;y<7;++y)for(int x=-3;x<1;++x) {
        const int count=kind=="empty"?0:kind=="emission"?1:2;image.set_deep_samples(x,y,0,count);
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
            image.set_deep_value(x,y,0,c,s,value);
        }
    }
    if(!image.write(path.u8string()))throw std::runtime_error(image.geterror());
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("expected new fixture directory");
        auto root=fs::absolute(fs::u8path(argv[1]));if(!fs::create_directory(root))throw std::runtime_error("fixture directory exists");
        nlohmann::json files=nlohmann::json::object();
        for(const char* kind:{"scalar","tiled","coloured","changed_alpha","beauty","missing_alpha","no_zback","reordered","empty","window","nonlinear","emission","volumes","flat"}) {
            auto path=root/(std::string(kind)+".exr");fixture(path,kind);files[kind]=path.u8string();
        }
        std::cout<<files.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
