#pragma once
#include <OpenImageIO/imageio.h>
#include <vector>

namespace thumbgen::detail {
struct ChannelRun {
    int first=0,end=0;
    std::size_t compact_first=0,byte_offset=0,pixel_bytes=0;
};
struct ChannelPlan {
    std::vector<int> channels;
    std::vector<ChannelRun> runs;
    std::vector<std::size_t> offsets,strides;
    std::size_t pixel_bytes=0,sample_bytes=0;
};
// Sorted unique channels, grouped into consecutive source ranges. Native data
// uses contiguous range planes; FLOAT colour uses compact interleaved pixels.
ChannelPlan channel_plan(const OIIO::ImageSpec& spec,const std::vector<int>& channels,bool native);
bool read_selected(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const ChannelPlan& plan,bool native,void* pixels);
}
