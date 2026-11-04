#pragma once
#include "data_image.h"
#include <optional>
#include <utility>
namespace thumbgen::detail {
struct SampledIdPlan {
    int channel=0,block_width=0,block_height=0;
    bool tiled=false;
    std::vector<int> x,y,x_blocks,y_blocks;
    std::size_t grid_bytes=0,scratch_bytes=0,peak_bytes=0;
};
// An absent plan retains the established full-source path. Plans are derived
// from every requested output, never from current pixel values.
std::optional<SampledIdPlan> sampled_id_plan(const OIIO::ImageSpec& spec,const std::string& format,
    const std::vector<int>& channels,const DataMapping& mapping,const std::vector<std::pair<int,int>>& sizes);
DataImage read_sampled_id(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const SampledIdPlan& plan,const DataMapping& mapping,std::size_t limit);
}
