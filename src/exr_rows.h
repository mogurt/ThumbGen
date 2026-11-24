#pragma once
#include "bounded_read.h"
#include <OpenImageIO/color.h>
#include <OpenImageIO/imageio.h>
#include <optional>
#include <string>
#include <vector>

namespace thumbgen::detail {
bool preflight_exr_header(const std::string& path);

struct ExrRowProbe {
    NativeReadPlan read;
    OIIO::TypeDesc sample_format;
    int components;
    bool unassociated_alpha;
};

// The selected part and channel order come from the normal selection policy.
// The factory presents native pixels in display order, with zero pixels
// outside the data window. Requests are relative to the display origin.
std::optional<ExrRowProbe> probe_exr_rows(const std::string& path,const OIIO::ImageSpec& spec,
    int part,const std::vector<int>& channels);

void exr_rows_to_linear(const ExrRowProbe& probe,const OIIO::ColorProcessorHandle& processor,
    const void* source,float* destination,std::size_t pixels);
void linearize_rows(const OIIO::ColorProcessorHandle& processor,float* pixels,
    std::size_t count,int components,bool straight_alpha);
}
