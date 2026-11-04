#include "selected_input.h"
#include "files.h"
#include <algorithm>
#include <limits>

namespace thumbgen::detail {
ChannelPlan channel_plan(const OIIO::ImageSpec& spec,const std::vector<int>& channels,bool native) {
    if(channels.empty() || std::any_of(channels.begin(),channels.end(),[&](int c){return c<0 || c>=spec.nchannels;}))
        throw FileFailure("visualization_layout_unsupported","invalid selected channels");
    ChannelPlan plan;plan.channels=channels;std::sort(plan.channels.begin(),plan.channels.end());
    plan.channels.erase(std::unique(plan.channels.begin(),plan.channels.end()),plan.channels.end());
    plan.offsets.resize(plan.channels.size());plan.strides.resize(plan.channels.size());
    for(std::size_t i=0;i<plan.channels.size();) {
        const auto begin=i;std::size_t bytes=0;
        do {
            auto type=native?spec.channelformat(plan.channels[i]):OIIO::TypeDesc(OIIO::TypeDesc::FLOAT);
            const bool numeric=type.is_floating_point() || (type.basetype>=OIIO::TypeDesc::UINT8 && type.basetype<=OIIO::TypeDesc::INT64);
            if(type.aggregate!=OIIO::TypeDesc::SCALAR || type.arraylen || !type.size() || type.size()>8 || !numeric)
                throw FileFailure("data_type_unsupported","unsupported selected channel type");
            plan.sample_bytes=std::max(plan.sample_bytes,type.size());plan.offsets[i]=bytes;bytes+=type.size();++i;
        }while(i<plan.channels.size() && plan.channels[i]==plan.channels[i-1]+1);
        plan.runs.push_back({plan.channels[begin],plan.channels[i-1]+1,begin,plan.pixel_bytes,bytes});
        for(auto c=begin;c<i;++c)plan.strides[c]=bytes;
        plan.pixel_bytes+=bytes;
    }
    // Whole range planes share one allocation. Keep each read contiguous even
    // for mixed native types: UNKNOWN with arbitrary strides is not reliable
    // across OIIO conversion paths. No FLOAT conversion is used for native IDs.
    const auto pixels=std::size_t(spec.width)*std::size_t(spec.height);
    if(spec.width<1 || spec.height<1 || pixels>std::numeric_limits<std::size_t>::max()/plan.pixel_bytes)
        throw FileFailure("decode_limit_exceeded","selected channel layout size overflow");
    for(const auto& run:plan.runs)for(std::size_t c=run.compact_first;c<run.compact_first+std::size_t(run.end-run.first);++c)
        plan.offsets[c]+=run.byte_offset*pixels;
    return plan;
}
bool read_selected(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const ChannelPlan& plan,bool native,void* pixels) {
    auto* bytes=static_cast<unsigned char*>(pixels);
    for(const auto& run:plan.runs) {
        if(native) {
            auto offset=run.byte_offset*std::size_t(spec.width)*std::size_t(spec.height);
            if(!input.read_image(part,0,run.first,run.end,OIIO::TypeDesc::UNKNOWN,bytes+offset))return false;
        } else if(plan.runs.size()==1) {
            if(!input.read_image(part,0,run.first,run.end,OIIO::TypeDesc::FLOAT,bytes))return false;
        } else {
            auto stride=OIIO::stride_t(plan.pixel_bytes);
            if(!input.read_image(part,0,run.first,run.end,OIIO::TypeDesc::FLOAT,
                bytes+run.compact_first*sizeof(float),stride,stride*spec.width))return false;
        }
    }
    return true;
}
}
