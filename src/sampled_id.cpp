#include "sampled_id.h"
#include "files.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
namespace thumbgen::detail {
namespace {
void unique(std::vector<int>& values) {
    std::sort(values.begin(),values.end());values.erase(std::remove(values.begin(),values.end(),-1),values.end());
    values.erase(std::unique(values.begin(),values.end()),values.end());
}
std::size_t multiply(std::size_t a,std::size_t b) {
    if(a && b>std::numeric_limits<std::size_t>::max()/a)throw FileFailure("decode_limit_exceeded","sample grid size overflows");
    return a*b;
}
}
std::optional<SampledIdPlan> sampled_id_plan(const OIIO::ImageSpec& spec,const std::string& format,
    const std::vector<int>& channels,const DataMapping& mapping,const std::vector<std::pair<int,int>>& sizes) {
    if(format!="openexr" || spec.deep || spec.depth!=1 || mapping.mode!=DataMode::id || channels.size()!=1 || sizes.empty())return {};
    const auto orientation=spec.get_int_attribute("Orientation",1);
    if(orientation<1 || orientation>8)return {}; // Preserve the established output-stage rejection.
    if(channels[0]<0 || channels[0]>=spec.nchannels)return {};
    const auto type=spec.channelformat(channels[0]);
    switch(type.basetype) {
        case OIIO::TypeDesc::UINT8:case OIIO::TypeDesc::INT8:case OIIO::TypeDesc::UINT16:case OIIO::TypeDesc::INT16:
        case OIIO::TypeDesc::UINT32:case OIIO::TypeDesc::INT32:break;
        default:return {}; // Floating and 64-bit values require full validation.
    }
    if(type.aggregate!=OIIO::TypeDesc::SCALAR || type.arraylen || spec.width<1 || spec.height<1)return {};
    SampledIdPlan plan;plan.channel=channels[0];plan.tiled=spec.tile_width>0;
    if(plan.tiled) {
        if(spec.tile_height<1 || spec.tile_depth!=1)return {};
        plan.block_width=spec.tile_width;plan.block_height=spec.tile_height;
    }else {
        auto compression=spec.get_string_attribute("compression");
        if(compression=="zip")plan.block_height=16;
        else if(compression=="piz")plan.block_height=32;
        else if(compression=="zips" || compression=="none" || compression=="rle")plan.block_height=1;
        else return {}; // Unmeasured compression retains full-source I/O.
        plan.block_width=spec.width;
    }
    for(const auto& size:sizes) {
        auto positions=categorical_coordinates(spec,size.first,size.second);
        if(plan.x.size()+positions.x.size()>262144 || plan.y.size()+positions.y.size()>262144)return {};
        plan.x.insert(plan.x.end(),positions.x.begin(),positions.x.end());plan.y.insert(plan.y.end(),positions.y.begin(),positions.y.end());
        unique(plan.x);unique(plan.y); // Bound metadata storage even with repeated output sizes.
    }
    unique(plan.x);unique(plan.y);
    for(int x:plan.x)plan.x_blocks.push_back(x/plan.block_width*plan.block_width);
    for(int y:plan.y)plan.y_blocks.push_back(y/plan.block_height*plan.block_height);
    unique(plan.x_blocks);unique(plan.y_blocks);
    const auto all_blocks=multiply((std::size_t(spec.width)-1)/plan.block_width+1,(std::size_t(spec.height)-1)/plan.block_height+1);
    const auto used_blocks=multiply(plan.x_blocks.size(),plan.y_blocks.size());
    if(used_blocks>all_blocks/2)return {}; // Dense sampling can be slower than full reads.
    plan.grid_bytes=multiply(multiply(plan.x.size(),plan.y.size()),type.size());
    plan.scratch_bytes=used_blocks?multiply(multiply(std::size_t(std::min(plan.block_width,spec.width)),std::min(plan.block_height,spec.height)),type.size()):0;
    if(plan.grid_bytes>std::numeric_limits<std::size_t>::max()-plan.scratch_bytes)throw FileFailure("decode_limit_exceeded","sample buffer sum overflows");
    const auto indices=multiply(plan.x.capacity()+plan.y.capacity()+plan.x.size()+plan.y.size()+plan.x_blocks.capacity()+plan.y_blocks.capacity(),sizeof(int));
    if(indices>std::numeric_limits<std::size_t>::max()-plan.grid_bytes-plan.scratch_bytes)throw FileFailure("decode_limit_exceeded","sample index sum overflows");
    plan.peak_bytes=std::max<std::size_t>(1,plan.grid_bytes+plan.scratch_bytes+indices);
    const auto full=multiply(multiply(std::size_t(spec.width),spec.height),type.size());
    if(plan.peak_bytes>full/2)return {};
    return plan;
}
DataImage read_sampled_id(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const SampledIdPlan& plan,const DataMapping& mapping,std::size_t limit) {
    if(plan.peak_bytes>limit)throw FileFailure("decode_limit_exceeded","sampled ID source buffers exceed limit");
    auto tick=std::chrono::steady_clock::now();
    DataImage result;result.sampled=true;result.sampled_x=plan.x;result.sampled_y=plan.y;result.source_spec=spec;result.mapping=mapping;
    result.decoded_bytes=plan.peak_bytes;result.types={spec.channelformat(plan.channel)};result.offsets={0};result.strides={result.types[0].size()};
    result.native.resize(plan.grid_bytes);std::vector<unsigned char> block(plan.scratch_bytes);const auto bytes=result.strides[0];
    for(int yb:plan.y_blocks)for(int xb:plan.x_blocks) {
        const auto xe=std::min(std::int64_t(xb)+plan.block_width,std::int64_t(spec.width));
        const auto ye=std::min(std::int64_t(yb)+plan.block_height,std::int64_t(spec.height));
        auto endpoint=[](std::int64_t origin,std::int64_t local) {
            auto value=origin+local;if(value<std::numeric_limits<int>::min() || value>std::numeric_limits<int>::max())throw FileFailure("invalid_dimensions","sample read endpoint overflows");return int(value);
        };
        bool ok=plan.tiled?input.read_tiles(part,0,endpoint(spec.x,xb),endpoint(spec.x,xe),endpoint(spec.y,yb),endpoint(spec.y,ye),spec.z,endpoint(spec.z,1),plan.channel,plan.channel+1,OIIO::TypeDesc::UNKNOWN,block.data()):
            input.read_scanlines(part,0,endpoint(spec.y,yb),endpoint(spec.y,ye),spec.z,plan.channel,plan.channel+1,OIIO::TypeDesc::UNKNOWN,block.data());
        if(!ok)throw FileFailure("decode_failed",input.geterror());
        for(auto y=std::lower_bound(plan.y.begin(),plan.y.end(),yb);y!=plan.y.end() && *y<ye;++y)
            for(auto x=std::lower_bound(plan.x.begin(),plan.x.end(),xb);x!=plan.x.end() && *x<xe;++x) {
                auto destination=(std::size_t(y-plan.y.begin())*plan.x.size()+std::size_t(x-plan.x.begin()))*bytes;
                auto source=(std::size_t(*y-yb)*std::size_t(xe-xb)+std::size_t(*x-xb))*bytes;
                std::memcpy(result.native.data()+destination,block.data()+source,bytes);
            }
    }
    result.decode_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count();return result;
}
}
