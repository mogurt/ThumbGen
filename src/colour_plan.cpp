#include "colour_plan.h"
#include "cms_input.h"
#include <cmath>
#include <cstring>
namespace thumbgen::detail {
ColourPlan colour_plan(const OIIO::ImageSpec& s,const std::string& format,
    bool eligible,bool layout,bool embedded,bool use_icc,const std::string& input) {
    ColourPlan p;
    auto fallback=[&](const char* reason){p.fallback=reason;return p;};
    if(!eligible)return fallback("Reference processing required by options or colour configuration");
    if(!layout || !s.channelformats.empty() || (s.format!=OIIO::TypeDesc::UINT8 && s.format!=OIIO::TypeDesc::UINT16) ||
       s.depth!=1 || s.x || s.y || s.full_x || s.full_y || s.width!=s.full_width || s.height!=s.full_height)
        return fallback("Decoded layout, alpha association, sample type or window requires reference processing");
    const bool ordinary=format=="png" || format=="jpeg" || format=="tiff" || format=="webp";
    const bool legacy=(format=="targa" || format=="bmp") && s.format==OIIO::TypeDesc::UINT8 &&
        (format!="bmp" || s.nchannels==3);
    if(!ordinary && !legacy)return fallback("Decoder format requires reference processing");
    const bool srgb=input=="sRGB" || input=="srgb_rec709_scene" || input=="sRGB Encoded Rec.709 (sRGB)";
    const bool gamma=input=="Gamma 2.2 Rec.709" || input=="g22_rec709_scene" || input=="Gamma 2.2 Encoded Rec.709";
    if(embedded && !use_icc)return fallback("Embedded ICC overridden by explicit input space");
    p.input=*skcms_sRGB_profile();
    if(use_icc) {
        auto* profile=s.find_attribute("ICCProfile");
        if(!profile || profile->type().basetype!=OIIO::TypeDesc::UINT8 || profile->datasize()<132 ||
           std::size_t(profile->datasize())>icc_profile_limit)return fallback("ICC requires reference validation");
        const int priority[]{1,0};
        if(!skcms_ParseWithA2BPriority(profile->data(),std::size_t(profile->datasize()),priority,2,&p.input) ||
           p.input.data_color_space!=0x52474220 || (p.input.pcs!=0x58595a20 && p.input.pcs!=0x4c616220))
            return fallback("ICC structure is unsupported by skcms");
        // LUT families remain on the reference engine until their category
        // passes the paired pixel acceptance suite; never select by asset ID.
        if(p.input.has_A2B)return fallback("RGB LUT category requires reference processing");
        if(!p.input.has_trc || !p.input.has_toXYZD50)return fallback("ICC matrix and TRC are unavailable");
    } else if(!srgb && !gamma)return fallback("Input transfer requires reference processing");
    else if(gamma) {const skcms_TransferFunction transfer{2.2f,1,0,0,0,0,0};skcms_SetTransferFunction(&p.input,&transfer);}
    p.sample_bytes=s.format==OIIO::TypeDesc::UINT16?2:1;
    p.route=!use_icc && srgb && p.sample_bytes==1?ColourPlan::Route::srgb8:ColourPlan::Route::integer_linear;
    return p;
}
void ColourPlan::transform(const void* source,float* output,std::size_t count,int channels) const {
    if(route!=Route::integer_linear || (channels!=3 && channels!=4))throw CmsFailure("invalid native colour plan");
    auto linear=*skcms_sRGB_profile();skcms_SetTransferFunction(&linear,skcms_Identity_TransferFunction());
    const auto format=sample_bytes==2?(channels==4?skcms_PixelFormat_RGBA_16161616LE:skcms_PixelFormat_RGB_161616LE):
        (channels==4?skcms_PixelFormat_RGBA_8888:skcms_PixelFormat_RGB_888);
    if(!skcms_Transform(source,format,skcms_AlphaFormat_Unpremul,&input,output,
        channels==4?skcms_PixelFormat_RGBA_ffff:skcms_PixelFormat_RGB_fff,
        skcms_AlphaFormat_PremulAsEncoded,&linear,count))throw CmsFailure("native skcms transform failed");
    for(std::size_t i=0;i<count*channels;++i)if(!std::isfinite(output[i]))throw CmsFailure("skcms produced non-finite RGB");
}
}
