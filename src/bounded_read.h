#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace thumbgen::detail {
// Planning is metadata-only. The factory runs once, after admission. The
// reader writes tightly packed native rows in increasing, contiguous order,
// with no sample conversion. The selected ImageSpec describes the data window,
// channel order, sample type, and alpha association. A one-part metadata probe
// can publish this spec before the decoder is opened.
// A tile/strip/frame reader owns its cache and declares that storage here.
class NativeRowSource {
public:
    virtual ~NativeRowSource()=default;
    virtual void read(int first,int last,void* destination)=0;
    virtual void finish()=0;
};
struct NativeReadPlan {
    unsigned pixel_bytes=0;
    std::uint64_t retained_bytes=0;
    std::uint64_t codec_bytes=0;
    std::function<std::unique_ptr<NativeRowSource>()> open;
};
struct BoundedReadMemory {
    std::uint64_t source=0,working=0,output=0,codec=0;
};
inline constexpr std::uint64_t bounded_mib=1024*1024;
inline constexpr std::size_t bounded_sampler_limit=128*bounded_mib;
inline constexpr std::size_t native_colour_block_pixels=1024*1024;
inline std::pair<int,int> fit_preview(int width,int height,int limit_width,int limit_height) {
    const double scale=std::min({double(limit_width)/width,double(limit_height)/height,1.0});
    return {std::max(1,int(std::lround(width*scale))),std::max(1,int(std::lround(height*scale)))};
}
inline std::uint64_t checked_bytes_add(std::uint64_t a,std::uint64_t b) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a)throw std::overflow_error("buffer size addition overflow");
    return a+b;
}
inline std::uint64_t checked_bytes_mul(std::uint64_t a,std::uint64_t b) {
    if(a && b>std::numeric_limits<std::uint64_t>::max()/a)throw std::overflow_error("buffer size multiplication overflow");
    return a*b;
}
inline std::uint64_t bounded_total(const BoundedReadMemory& m) {
    return checked_bytes_add(checked_bytes_add(m.source,m.working),checked_bytes_add(m.output,m.codec));
}
// Both preview buffers may be live during the first output's finish. The
// existing output envelope covers colour, orient, composite and encode copies.
inline BoundedReadMemory bounded_read_memory(std::uint64_t width,std::uint64_t height,unsigned channels,
    unsigned native_pixel_bytes,std::uint64_t retained,std::uint64_t codec,std::uint64_t cms,
    std::uint64_t sampler,const std::vector<std::pair<int,int>>& sizes) {
    if(!width || !height || width>65536 || height>65536 || (channels!=3 && channels!=4) ||
       !native_pixel_bytes || native_pixel_bytes>32 || !sampler || sizes.empty() || sizes.size()>2)
        throw std::invalid_argument("invalid bounded read layout");
    const auto block=std::min<std::uint64_t>(width*height,native_colour_block_pixels);
    BoundedReadMemory m;
    m.source=checked_bytes_add(checked_bytes_add(retained,cms),(block+width)*native_pixel_bytes);
    m.working=checked_bytes_add(sampler,block*channels*sizeof(float));
    std::uint64_t largest=0;
    for(auto [w,h]:sizes) {
        if(w<=0 || h<=0 || std::uint64_t(w)>width || std::uint64_t(h)>height)
            throw std::invalid_argument("invalid bounded preview size");
        const auto pixels=std::uint64_t(w)*std::uint64_t(h);
        largest=std::max(largest,pixels);
        m.output+=pixels*channels*sizeof(float);
    }
    m.output+=largest*5*4*sizeof(float);
    m.codec=checked_bytes_add(codec,largest*8+bounded_mib);
    return m;
}
} // namespace thumbgen::detail
