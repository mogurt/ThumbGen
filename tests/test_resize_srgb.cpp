#include "resize_srgb.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void close(float a,float b){if(std::abs(a-b)>.0001f)throw std::runtime_error("linear resize reference mismatch");}
int main(){try{
    OIIO::ImageSpec rgb(2,1,3,OIIO::TypeDesc::UINT8);
    unsigned char checker[]={0,0,0,255,255,255};OIIO::ImageBuf image(rgb,checker);
    auto resized=thumbgen::detail::resize_srgb8(image,1,1);
    float pixel[4];resized.getpixel(0,0,pixel);for(int c=0;c<3;++c)close(pixel[c],.5f);
    OIIO::ImageSpec rgba(2,1,4,OIIO::TypeDesc::UINT8);rgba.alpha_channel=3;
    unsigned char hidden[]={255,0,0,0,0,255,0,255};OIIO::ImageBuf transparent(rgba,hidden);
    auto associated=thumbgen::detail::resize_srgb8(transparent,1,1);
    associated.getpixel(0,0,pixel);close(pixel[0],0);close(pixel[1],.5f);close(pixel[2],0);close(pixel[3],.5f);
    auto identity=thumbgen::detail::resize_srgb8(transparent,2,1);
    identity.getpixel(0,0,pixel);for(float value:pixel)close(value,0);
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
