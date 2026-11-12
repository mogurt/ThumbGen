#include "resize_srgb.h"
#include "files.h"
#ifdef _MSC_VER
#pragma warning(push,0)
#endif
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../third_party/stb/stb_image_resize2.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace thumbgen::detail {
OIIO::ImageBuf resize_srgb8(const OIIO::ImageBuf& source,int width,int height) {
    const int channels=source.nchannels();
    if(source.spec().format!=OIIO::TypeDesc::UINT8 || !source.contiguous() ||
       !source.localpixels() || (channels!=3 && channels!=4) || width<1 || height<1)
        throw FileFailure("resize_failed","expected contiguous UINT8 RGB or RGBA input");
    OIIO::ImageSpec spec(width,height,channels,OIIO::TypeDesc::FLOAT);
    spec.alpha_channel=channels==4?3:-1;
    OIIO::ImageBuf output(spec);
    if(!output.localpixels())throw FileFailure("resize_failed","cannot allocate linear preview");
    STBIR_RESIZE resize;
    stbir_resize_init(&resize,source.localpixels(),source.spec().width,source.spec().height,0,
        output.localpixels(),width,height,0,channels==4?STBIR_RGBA:STBIR_RGB,STBIR_TYPE_UINT8_SRGB);
    stbir_set_datatypes(&resize,STBIR_TYPE_UINT8_SRGB,STBIR_TYPE_FLOAT);
    if(channels==4 && !stbir_set_pixel_layouts(&resize,STBIR_RGBA,STBIR_RGBA_PM))
        throw FileFailure("resize_failed","cannot configure associated alpha output");
    stbir_set_filters(&resize,STBIR_FILTER_TRIANGLE,STBIR_FILTER_TRIANGLE);
    stbir_set_edgemodes(&resize,STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP);
    if(!stbir_resize_extended(&resize))throw FileFailure("resize_failed","sRGB resize failed");
    return output;
}
}
