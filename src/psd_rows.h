#pragma once

#include "bounded_read.h"
#include <OpenImageIO/imageio.h>
#include <optional>
#include <string>

namespace thumbgen::detail {
struct PsdProbe {
    OIIO::ImageSpec spec;
    NativeReadPlan read;
    bool has_icc=false;
    unsigned compression=0;
};

// Plans only the PSD v1 RGB composite. No layer records or pixels are decoded.
std::optional<PsdProbe> probe_psd_rows(const std::string& input,std::uint64_t source_size,
    std::uint64_t decode_budget,std::size_t icc_limit,bool load_icc);
}
