#include "colour_plan.h"
#include "cms_input.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
namespace thumbgen::detail {
ColourPlan colour_plan(const OIIO::ImageSpec& s,const std::string& format,
    bool eligible,bool layout,bool embedded,bool use_icc,const std::string& input) {
    ColourPlan p;
    auto fallback=[&](const char* reason){p.route=ColourPlan::Route::reference;p.sample_bytes=4;p.fallback=reason;return p;};
    if(!eligible)return fallback("Reference processing required by options or colour configuration");
    if(!layout || !s.channelformats.empty() || (s.format!=OIIO::TypeDesc::UINT8 && s.format!=OIIO::TypeDesc::UINT16) ||
       s.depth!=1 || s.x || s.y || s.full_x || s.full_y || s.width!=s.full_width || s.height!=s.full_height)
        return fallback("Decoded layout, alpha association, sample type or window requires reference processing");
    const bool ordinary=format=="png" || format=="jpeg" || format=="tiff" || format=="webp";
    const bool legacy=(format=="targa" || format=="bmp") && s.format==OIIO::TypeDesc::UINT8 &&
        (format!="bmp" || (s.nchannels==3 && s.get_int_attribute("bmp:bitsperpixel",24)!=16)) &&
        (format!="targa" || s.get_int_attribute("oiio:BitsPerSample",8)==8);
    if(!ordinary && !legacy)return fallback("Decoder format requires reference processing");
    const bool srgb=input=="sRGB" || input=="srgb_rec709_scene" || input=="sRGB Encoded Rec.709 (sRGB)";
    const bool gamma=input=="Gamma 2.2 Rec.709" || input=="g22_rec709_scene" || input=="Gamma 2.2 Encoded Rec.709";
    if(embedded && !use_icc)return fallback("Embedded ICC overridden by explicit input space");
    p.input=*skcms_sRGB_profile();
    if(use_icc) {
        auto* profile=s.find_attribute("ICCProfile");
        if(!profile || profile->type().basetype!=OIIO::TypeDesc::UINT8 || profile->datasize()<132 ||
           std::size_t(profile->datasize())>icc_profile_limit)return fallback("ICC requires reference validation");
        const auto bytes=std::size_t(profile->datasize());
        p.profile_bytes=std::make_shared<std::vector<unsigned char>>(bytes+4,0);
        std::memcpy(p.profile_bytes->data(),profile->data(),bytes);
        const int priority[]{1,0};
        if(!skcms_ParseWithA2BPriority(p.profile_bytes->data(),p.profile_bytes->size(),priority,2,&p.input) ||
           p.input.data_color_space!=0x52474220 || (p.input.pcs!=0x58595a20 && p.input.pcs!=0x4c616220))
            return fallback("ICC structure is unsupported by skcms");
        const auto* raw=static_cast<const unsigned char*>(profile->data());
        auto be32=[](const unsigned char* b){return (std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|(std::uint32_t(b[2])<<8)|b[3];};
        const auto size=std::size_t(profile->datasize());
        const auto tags=be32(raw+128);
        if(tags>(size-132)/12)return fallback("ICC tag table requires reference validation");
        bool lut=false;
        for(std::uint32_t i=0;i<tags;++i) {
            const auto* tag=raw+132+i*12;const auto signature=be32(tag);
            // LCMS and skcms do not have identical extended-tag precedence.
            if(signature==0x63696370 || (signature>=0x44324230 && signature<=0x44324233))
                return fallback("ICC extended transform tags require LittleCMS");
            if(signature==0x41324230 || signature==0x41324231)lut=true;
        }
        // LUT families remain on the reference engine until their category
        // passes the paired pixel acceptance suite; never select by asset ID.
        if(lut || p.input.has_A2B)return fallback("RGB LUT category requires reference processing");
        if(!p.input.has_trc || !p.input.has_toXYZD50)return fallback("ICC matrix and TRC are unavailable");
    } else if(!srgb && !gamma)return fallback("Input transfer requires reference processing");
    else if(gamma) {const skcms_TransferFunction transfer{2.2f,1,0,0,0,0,0};skcms_SetTransferFunction(&p.input,&transfer);}
    p.sample_bytes=s.format==OIIO::TypeDesc::UINT16?2:1;
    p.route=!use_icc && srgb && p.sample_bytes==1?ColourPlan::Route::srgb8:ColourPlan::Route::integer_linear;
    if(p.route==ColourPlan::Route::integer_linear) {
        // Parsing alone does not prove that the transform program is supported.
        // Probe before allocation/admission so valid unsupported profiles use
        // the complete reference footprint and decode path deterministically.
        const std::uint16_t sample[4]{};float output[4]{};
        try {p.transform(sample,output,1,s.nchannels);}
        catch(const CmsFailure&){return fallback("ICC transform program is unsupported by skcms");}
    }
    return p;
}
void ColourPlan::transform(const void* source,float* output,std::size_t count,int channels) const {
    if(route!=Route::integer_linear || (channels!=3 && channels!=4))throw CmsFailure("invalid native colour plan");
    auto linear=*skcms_sRGB_profile();skcms_SetTransferFunction(&linear,skcms_Identity_TransferFunction());
    const auto format=sample_bytes==2?(channels==4?skcms_PixelFormat_RGBA_16161616LE:skcms_PixelFormat_RGB_161616LE):
        (channels==4?skcms_PixelFormat_RGBA_8888:skcms_PixelFormat_RGB_888);
    // skcms accepts at most INT_MAX bytes per call. Bound each call so a
    // valid large decoded source does not become an engine-specific failure.
    const auto* bytes=static_cast<const unsigned char*>(source);
    for(std::size_t base=0;base<count;) {
        const auto n=std::min<std::size_t>(count-base,1024*1024);
        if(!skcms_Transform(bytes+base*channels*sample_bytes,format,skcms_AlphaFormat_Unpremul,&input,
            output+base*channels,channels==4?skcms_PixelFormat_RGBA_ffff:skcms_PixelFormat_RGB_fff,
            skcms_AlphaFormat_PremulAsEncoded,&linear,n))throw CmsFailure("native skcms transform failed");
        base+=n;
    }
    for(std::size_t i=0;i<count*channels;++i)if(!std::isfinite(output[i]))throw CmsFailure("skcms produced non-finite RGB");
}
}
