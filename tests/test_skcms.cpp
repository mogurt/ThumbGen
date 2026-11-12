#include "skcms.h"
#include <cmath>
#include <iostream>
#include <cstdint>
int main() {
    auto linear=*skcms_sRGB_profile();
    skcms_SetTransferFunction(&linear,skcms_Identity_TransferFunction());
    const std::uint16_t source[]{0,32768,65535,32768};float result[4]{};
    if(!skcms_Transform(source,skcms_PixelFormat_RGBA_16161616LE,skcms_AlphaFormat_Unpremul,
        skcms_sRGB_profile(),result,skcms_PixelFormat_RGBA_ffff,skcms_AlphaFormat_PremulAsEncoded,&linear,1))return 1;
    const float a=32768.f/65535.f;
    const float middle=std::pow((a+0.055f)/1.055f,2.4f)*a;
    if(result[0]!=0 || std::abs(result[1]-middle)>0.0001f || std::abs(result[2]-a)>0.0001f || result[3]!=a) {
        std::cerr<<"native linear transform/alpha mismatch\n";return 1;
    }
    return 0;
}
