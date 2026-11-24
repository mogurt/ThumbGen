#pragma once
#include "bounded_read.h"
#include <OpenImageIO/imageio.h>
#include <filesystem>

namespace thumbgen::detail {
struct PngRowPlan {
    NativeReadPlan read;
    std::uint64_t fallback_codec_bytes=0;
    bool retains_frame=false;
};

// Before ImageInput::open, check the PNG header/metadata footprint and the
// minimum native frame size for Adam7.
bool preflight_png_header(const std::filesystem::path& path,std::uint64_t budget);

// The metadata reader remains open through the row source's lifetime.
PngRowPlan plan_png_rows(const std::filesystem::path& path,OIIO::ImageInput& input,
    const OIIO::ImageSpec& spec,std::uint64_t budget);
}
