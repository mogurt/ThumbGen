#include "lut_profiles.h"
#include "colour_plan.h"
#include "resize_srgb.h"
#include <thumbgen/core.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/color.h>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <fstream>
#include <cstring>
namespace fs=std::filesystem;
void require(bool v,const std::string& m){if(!v)throw std::runtime_error(m);}
int main(int argc,char** argv){try {
    auto root=fs::temp_directory_path()/("thumbgen-lut-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    thumbgen::Json results=thumbgen::Json::array();thumbgen::Engine product,reference;thumbgen::Engine::Control control;control.reference_colour=true;reference.set_control(control);OIIO::ColorConfig config("ocio://default");
    for(const std::string family:{"mft1","mft2","mAB"})for(bool lab:{false,true}) {
        auto bytes=lut_fixture::profile(family,lab);thumbgen::detail::ColourPlan candidate;candidate.route=thumbgen::detail::ColourPlan::Route::integer_linear;candidate.sample_bytes=2;const int priority[]{1,0};
        require(skcms_ParseWithA2BPriority(bytes.data(),bytes.size(),priority,2,&candidate.input) && candidate.input.has_A2B,"fixture parse "+family);
        int maximum=0;double worst_mae=0;thumbgen::Json layouts=thumbgen::Json::array();
        for(int nc:{3,4}) {
            OIIO::ImageSpec s(129,65,nc,OIIO::TypeDesc::UINT16);s.alpha_channel=nc==4?3:-1;s.attribute("oiio:UnassociatedAlpha",1);s.attribute("ICCProfile",OIIO::TypeDesc(OIIO::TypeDesc::UINT8,int(bytes.size())),bytes.data());OIIO::ImageBuf image(s);auto* source=static_cast<unsigned short*>(image.localpixels());
            for(int i=0;i<129*65;++i){for(int c=0;c<3;++c)source[i*nc+c]=static_cast<unsigned short>((i*977+c*21721)%65536);if(nc==4)source[i*nc+3]=static_cast<unsigned short>((i%17)*4095);}
            const auto name=family+(lab?"-Lab":"-XYZ")+std::to_string(nc);auto input=root/(name+".png");require(image.write(input.u8string()),image.geterror());
            thumbgen::Job j;j.input=input.u8string();j.options.background="transparent";j.options.threads=1;j.outputs={{(root/(name+"-ref.png")).u8string(),"png",32,32,90}};auto ref=reference.convert(j);require(ref.value("ok",false),ref.dump());
            j.outputs[0].path=(root/(name+"-product.png")).u8string();auto actual_product=product.convert(j);
            require(actual_product.value("ok",false) && actual_product["colour"]["processing"]=="reference" && actual_product["colour"]["icc"]["engine"]=="LittleCMS2 2.20",actual_product.dump());
            OIIO::ImageBuf product_image(j.outputs[0].path),reference_image(ref["outputs"][0]["path"].get<std::string>());
            require(product_image.read(0,0,true,OIIO::TypeDesc::UINT8) && reference_image.read(0,0,true,OIIO::TypeDesc::UINT8),"fallback readback");
            require(product_image.spec().image_bytes()==reference_image.spec().image_bytes() && std::memcmp(product_image.localpixels(),reference_image.localpixels(),product_image.spec().image_bytes())==0,"LUT fallback differs");
            j.outputs[0].path=ref["outputs"][0]["path"].get<std::string>();
            OIIO::ImageSpec ls(129,65,nc,OIIO::TypeDesc::FLOAT);ls.alpha_channel=nc==4?3:-1;OIIO::ImageBuf linear(ls);OIIO::ImageSpec read_config;read_config.attribute("oiio:UnassociatedAlpha",1);auto decoder=OIIO::ImageInput::open(input.u8string(),&read_config);std::vector<unsigned short> decoded(129*65*nc);require(decoder && decoder->read_image(0,0,0,nc,OIIO::TypeDesc::UINT16,decoded.data()),"native decode");decoder->close();
            candidate.transform(decoded.data(),static_cast<float*>(linear.localpixels()),129*65,nc);auto preview=thumbgen::detail::resize_linear(linear,32,16);
            require(OIIO::ImageBufAlgo::colorconvert(preview,preview,config.resolve("lin_rec709_scene"),config.resolve("srgb_rec709_scene"),true,"","",&config),preview.geterror());
            if(nc==4)require(OIIO::ImageBufAlgo::unpremult(preview,preview),preview.geterror());
            OIIO::ImageBuf expected(j.outputs[0].path);require(expected.read(0,0,true,OIIO::TypeDesc::UINT8),expected.geterror());
            require(expected.spec().width==32 && expected.spec().height==16,"dimensions");
            const auto* actual=static_cast<const float*>(preview.localpixels());auto output_decoder=OIIO::ImageInput::open(j.outputs[0].path,&read_config);std::vector<unsigned char> target_pixels(32*16*nc);require(output_decoder && output_decoder->read_image(0,0,0,nc,OIIO::TypeDesc::UINT8,target_pixels.data()),"straight output readback");output_decoder->close();auto* target=target_pixels.data();double error=0;int layout_max=0;
            for(int i=0;i<32*16*nc;++i){int q=int(std::floor(std::clamp(actual[i],0.f,1.f)*255.f+0.5f));int delta=std::abs(q-int(target[i]));if(nc==4 && i%4==3)require(delta==0,"alpha mismatch");else {maximum=std::max(maximum,delta);layout_max=std::max(layout_max,delta);error+=delta;}}
            worst_mae=std::max(worst_mae,error/(32*16*3));layouts.push_back({{"channels",nc},{"max",layout_max},{"mae",error/(32*16*3)}});
        }
        results.push_back({{"layouts",layouts},{"family",family},{"pcs",lab?"Lab":"XYZ"},{"max_rgb8_error",maximum},{"worst_image_mae",worst_mae},{"passes",maximum<=3 && worst_mae<=0.2}});
    }
    if(argc==2){std::ofstream report(argv[1]);report<<results.dump(2)<<'\n';}std::cout<<results.dump(2)<<'\n';fs::remove_all(root);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
