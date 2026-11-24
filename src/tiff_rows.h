#pragma once
#include "bounded_read.h"
#include <OpenImageIO/imageio.h>
#include <filesystem>

namespace thumbgen::detail {
struct TiffRowPlan {
    NativeReadPlan read;
    std::uint64_t fallback_codec_bytes=0;
};

// Check the directories OIIO will inspect using the locked libtiff allocator
// limits before opening OIIO's metadata reader.
void preflight_tiff_header(const std::filesystem::path& path,std::uint64_t budget);

TiffRowPlan plan_tiff_rows(const std::filesystem::path& path,const OIIO::ImageSpec& spec,
    int part,std::uint64_t budget);
}
