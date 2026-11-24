#pragma once
#include "bounded_read.h"
#include <OpenImageIO/imageio.h>
#include <optional>
#include <string>

namespace thumbgen::detail {
struct JpegProbe {
    OIIO::ImageSpec spec;
    NativeReadPlan read;
    std::uint64_t codec_bytes=0;
    bool has_icc=false;
    bool multiscan=false;
};
// SOI detection is independent of the filename. A recognized JPEG with an
// invalid header throws. The row factory owns the prepared decoder and leaves
// its first scan untouched until the controller admits the task.
std::optional<JpegProbe> probe_jpeg(const std::string& path,std::uint64_t budget,
    std::size_t icc_limit=std::numeric_limits<std::size_t>::max());
}
