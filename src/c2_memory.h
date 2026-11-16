#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
namespace thumbgen::detail {
inline constexpr std::uint64_t c2_workspace_limit=32ULL*1024*1024;
struct C2Memory {
    std::uint64_t source,working,output,codec;
    std::uint64_t dynamic()const{return source+working+output+codec;}
};
// Shared execution/admission envelope. Geometry is deliberately bounded before
// multiplication. Counts include two previews and all existing finish overlap;
// the 32MiB codec allowance is additional to worker/engine reservation. Native
// blocks/rows use the UINT16 worst case also for eligible UINT8 ICC/Gamma input.
inline C2Memory c2_memory(std::uint64_t w,std::uint64_t h,std::uint64_t channels,std::uint64_t ow,std::uint64_t oh,std::uint64_t cms=0){
    if(!w || !h || w>65536 || h>65536 || w*h<8000000 || (channels!=3 && channels!=4) ||
       !ow || !oh || ow>=w || oh>=h || ow*oh>960ULL*960 || cms>64ULL*1024*1024)
        throw std::invalid_argument("invalid C2 memory geometry");
    auto block=std::min<std::uint64_t>(w*h,1048576),preview=ow*oh;
    return {block*channels*2+2*w*channels*2+cms,
            c2_workspace_limit+block*channels*sizeof(float),
            preview*5*4*sizeof(float),
            32ULL*1024*1024+preview*8+1024*1024};
}
}
