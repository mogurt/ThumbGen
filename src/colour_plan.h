#pragma once
#include "skcms.h"
#include <OpenImageIO/imageio.h>
#include <string>
#include <memory>
#include <vector>
namespace thumbgen::detail {
// Built once from selected decoder metadata, before allocating pixels. Its
// storage choice is also the worker's admission footprint.
struct ColourPlan {
    enum class Route {reference,srgb8,integer_linear};
    Route route=Route::reference;
    unsigned sample_bytes=4;
    // Stable ownership across returned/copyable plans; skcms curves point here.
    // Included in the pre-existing four-profile CMS metadata reservation.
    std::shared_ptr<std::vector<unsigned char>> profile_bytes;
    skcms_ICCProfile input{};
    std::string fallback;
    const char* processing() const {return route==Route::srgb8?"srgb8-linear-v1":route==Route::integer_linear?"integer-linear-v1":"reference";}
    bool linear() const {return route!=Route::reference;}
    void transform(const void* source,float* output,std::size_t pixels,int channels) const;
};
ColourPlan colour_plan(const OIIO::ImageSpec& spec,const std::string& format,
    bool eligible,bool layout,bool embedded,bool use_icc,const std::string& input);
}
