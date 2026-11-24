#pragma once
#include "bounded_read.h"
#include <OpenImageIO/imageio.h>
#include <optional>
#include <string>

namespace thumbgen::detail {
struct OtherRowsProbe {
    std::string format;
    // With a spec, selection can proceed without an open decoder. With no
    // spec, the existing OIIO metadata/selection path is preflighted for use.
    std::optional<OIIO::ImageSpec> spec;
    NativeReadPlan read;
    // Complete-image route only. Includes any native frame also retained
    // by the bounded route and excludes the client's own decoded raster.
    std::uint64_t fallback_codec_bytes=0;
};

std::optional<OtherRowsProbe> probe_other_rows(const std::string& path,std::uint64_t file_bytes,
    std::uint64_t budget,const OIIO::ImageSpec& config,std::size_t icc_limit);
}
