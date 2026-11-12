#pragma once
#include <lcms2.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
namespace lut_fixture {
using Bytes=std::vector<unsigned char>;
inline void u16(Bytes& b,unsigned v){b.push_back(static_cast<unsigned char>(v>>8));b.push_back(static_cast<unsigned char>(v));}
inline void u32(Bytes& b,unsigned v){u16(b,v>>16);u16(b,v&65535);}
inline void at32(Bytes& b,int at,unsigned v){for(int i=3;i>=0;--i){b[at+i]=static_cast<unsigned char>(v);v>>=8;}}
inline Bytes tag(const std::string& family,bool lab,bool perceptual=false) {
    const bool eight=family=="mft1",mab=family=="mAB";const int grid=17;
    Bytes b;
    if(mab) {
        b.resize(32);b[0]='m';b[1]='A';b[2]='B';b[3]=' ';b[8]=b[9]=3;
        at32(b,12,68);at32(b,24,104);at32(b,28,32);
        for(int i=0;i<6;++i){u32(b,0x63757276);u32(b,0);u32(b,0);} // identity A/B curves
        b.resize(124);b[104]=b[105]=b[106]=grid;b[120]=2;
    } else {
        b.resize(eight?48:52);b[0]='m';b[1]='f';b[2]='t';b[3]=eight?'1':'2';b[8]=b[9]=3;b[10]=grid;
        for(int i=0;i<3;++i)at32(b,12+(i*3+i)*4,65536);
        if(!eight){b[48]=b[50]=1;}
        for(int c=0;c<3;++c)for(int i=0;i<256;++i){if(eight)b.push_back(static_cast<unsigned char>(i));else u16(b,i*257);}
    }
    for(int r=0;r<grid;++r)for(int g=0;g<grid;++g)for(int bl=0;bl<grid;++bl) {
        const double red=std::pow(double(r)/(grid-1),2.2),green=std::pow(double(g)/(grid-1),2.2),blue=std::pow(double(bl)/(grid-1),2.2);
        cmsCIEXYZ xyz{0.4360747*red+0.3850649*green+0.1430804*blue,0.2225045*red+0.7168786*green+0.0606169*blue,0.0139322*red+0.0971045*green+0.7141733*blue};
        if(perceptual){xyz.X*=0.8;xyz.Y*=0.8;xyz.Z*=0.8;}
        double values[]{xyz.X/2,xyz.Y/2,xyz.Z/2};
        if(lab){cmsCIELab l;cmsXYZ2Lab(cmsD50_XYZ(),&l,&xyz);values[0]=l.L/100;values[1]=(l.a+128)/255;values[2]=(l.b+128)/255;}
        for(double v:values){v=std::clamp(v,0.0,1.0);if(eight)b.push_back(static_cast<unsigned char>(std::lround(v*255)));else u16(b,unsigned(std::lround(v*(lab&&!mab?65280:65535))));}
    }
    if(!mab)for(int c=0;c<3;++c)for(int i=0;i<256;++i){if(eight)b.push_back(static_cast<unsigned char>(i));else u16(b,i*257);}
    return b;
}
inline Bytes profile(const std::string& family,bool lab) {
    auto p=cmsCreateProfilePlaceholder(nullptr);cmsSetProfileVersion(p,family=="mAB"?4.3:2.1);cmsSetDeviceClass(p,cmsSigInputClass);cmsSetColorSpace(p,cmsSigRgbData);cmsSetPCS(p,lab?cmsSigLabData:cmsSigXYZData);cmsSetHeaderRenderingIntent(p,INTENT_PERCEPTUAL);
    cmsWriteTag(p,cmsSigMediaWhitePointTag,cmsD50_XYZ());auto relative=tag(family,lab),perceptual=tag(family,lab,true);
    cmsWriteRawTag(p,cmsSigAToB1Tag,relative.data(),static_cast<cmsUInt32Number>(relative.size()));cmsWriteRawTag(p,cmsSigAToB0Tag,perceptual.data(),static_cast<cmsUInt32Number>(perceptual.size()));
    cmsUInt32Number size=0;cmsSaveProfileToMem(p,nullptr,&size);Bytes bytes(size);cmsSaveProfileToMem(p,bytes.data(),&size);cmsCloseProfile(p);return bytes;
}
}
