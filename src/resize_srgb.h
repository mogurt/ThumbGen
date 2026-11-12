#pragma once
#include <OpenImageIO/imagebuf.h>

namespace thumbgen::detail {
// Straight UINT8 sRGB input -> associated FLOAT linear Rec.709 output.
// Encoding and final UNORM8 quantization remain in the shared output path.
OIIO::ImageBuf resize_srgb8(const OIIO::ImageBuf& source,int width,int height);
OIIO::ImageBuf resize_linear(const OIIO::ImageBuf& source,int width,int height);
}
