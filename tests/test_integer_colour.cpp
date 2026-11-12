#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <lcms2.h>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cmath>
namespace fs=std::filesystem;
void require(bool v,const std::string& message){if(!v)throw std::runtime_error(message);}
int main(){try {
    const auto root=fs::temp_directory_path()/("thumbgen-integer-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    thumbgen::Engine fast,reference;thumbgen::Engine::Control control;control.reference_colour=true;reference.set_control(control);
    auto profile=cmsCreate_sRGBProfile();cmsUInt32Number bytes=0;cmsSaveProfileToMem(profile,nullptr,&bytes);std::vector<unsigned char> icc(bytes);cmsSaveProfileToMem(profile,icc.data(),&bytes);cmsCloseProfile(profile);
    int id=0;
    for(const std::string ext:{"png","tif","tga","bmp","jpeg","webp"})for(int bits:{8,16})for(int nc:{3,4})for(int transfer:{0,1,2,3}) {
        if((ext=="jpeg" && (bits==16 || nc==4)) || (ext=="webp" && bits==16))continue;
        if((ext=="tga" || ext=="bmp") && (bits==16 || transfer!=0 || (ext=="bmp" && nc==4)))continue;
        auto name=std::to_string(id++);OIIO::ImageSpec s(127,65,nc,bits==16?OIIO::TypeDesc::UINT16:OIIO::TypeDesc::UINT8);s.alpha_channel=nc==4?3:-1;s.attribute("oiio:UnassociatedAlpha",1);
        if(ext=="tif")s.attribute("Orientation",1+(id%8));
        if(ext=="tga")s.attribute("compression","rle");
        if(transfer==3) {
            cmsCIExyY white{0.3127,0.3290,1};cmsCIExyYTRIPLE primaries{{0.64,0.33,1},{0.21,0.71,1},{0.15,0.06,1}};
            auto* gamma=cmsBuildGamma(nullptr,2.2);cmsToneCurve* curves[]{gamma,gamma,gamma};
            auto wide=cmsCreateRGBProfile(&white,&primaries,curves);cmsUInt32Number n=0;cmsSaveProfileToMem(wide,nullptr,&n);icc.resize(n);cmsSaveProfileToMem(wide,icc.data(),&n);cmsCloseProfile(wide);cmsFreeToneCurve(gamma);
        }
        if(transfer>=2)s.attribute("ICCProfile",OIIO::TypeDesc(OIIO::TypeDesc::UINT8,int(icc.size())),icc.data());
        OIIO::ImageBuf source(s);std::vector<float> values(127*65*nc);
        for(int i=0;i<127*65;++i){for(int c=0;c<3;++c)values[i*nc+c]=float((i*17+c*61)%257)/256; if(nc==4)values[i*nc+3]=float(i%17)/16;}
        require(source.set_pixels(source.roi(),OIIO::TypeDesc::FLOAT,values.data()),"fixture pixels");auto path=root/(name+"."+ext);require(source.write(path.u8string()),source.geterror());
        for(const std::string background:{"black","white","transparent"}) {
            thumbgen::Job j;j.input=path.u8string();j.options.threads=1;j.options.background=background;j.options.input_space=transfer==1?"g22_rec709_scene":transfer>=2?"auto":"sRGB";
            j.outputs={{(root/(name+background+"-a.png")).u8string(),"png",32,32,90},{(root/(name+background+"-small-a.png")).u8string(),"png",13,11,90}};
            auto a=fast.convert(j);require(a.value("ok",false),a.dump());
            require(a["colour"]["processing"]==(bits==8 && transfer==0?"srgb8-linear-v1":"integer-linear-v1"),a.dump());
            if(transfer>=2)require(a["colour"]["icc"]["engine"]=="skcms",a.dump());
            j.outputs[0].path=(root/(name+background+"-b.png")).u8string();j.outputs[1].path=(root/(name+background+"-small-b.png")).u8string();auto b=reference.convert(j);require(b.value("ok",false),b.dump());
            for(int out=0;out<2;++out) {
                OIIO::ImageBuf x(a["outputs"][out]["path"].get<std::string>()),y(b["outputs"][out]["path"].get<std::string>());require(x.read(0,0,true,OIIO::TypeDesc::UINT8) && y.read(0,0,true,OIIO::TypeDesc::UINT8),"readback");
                require(x.spec().image_bytes()==y.spec().image_bytes(),"size mismatch");OIIO::ImageSpec read_config;read_config.attribute("oiio:UnassociatedAlpha",1);
                auto xd=OIIO::ImageInput::open(a["outputs"][out]["path"].get<std::string>(),&read_config),yd=OIIO::ImageInput::open(b["outputs"][out]["path"].get<std::string>(),&read_config);
                std::vector<unsigned char> xv(x.spec().image_bytes()),yv(y.spec().image_bytes());
                require(xd && yd && xd->read_image(0,0,0,x.nchannels(),OIIO::TypeDesc::UINT8,xv.data()) && yd->read_image(0,0,0,y.nchannels(),OIIO::TypeDesc::UINT8,yv.data()),"straight readback");xd->close();yd->close();
                auto* xp=xv.data();auto* yp=yv.data();int max=0;
                for(std::size_t i=0;i<x.spec().image_bytes();++i){max=std::max(max,std::abs(int(xp[i])-int(yp[i])));if(x.nchannels()==4 && i%4==3)require(xp[i]==yp[i],"alpha mismatch "+name);}
                require(max<=1,"pixel error "+name+" "+ext+" "+std::to_string(bits)+" "+std::to_string(transfer)+" "+background+": "+std::to_string(max));
            }
        }
    }
    fs::remove_all(root);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
