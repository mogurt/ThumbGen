#pragma once
#include "colour_plan.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace thumbgen::detail {
inline constexpr std::size_t colour_chunk_pixels=1024*1024;
inline constexpr std::uint64_t png_scanline_min_pixels=8000000;

// OIIO 3.2.1.2 keeps PNG interlace private (pnginput.cpp); no ImageSpec
// attribute proves its absence. Inspect the fixed IHDR only AFTER the decoded
// format/layout is known. Unknown or inconsistent headers keep the old reader.
inline bool png_scanline_eligible(const std::filesystem::path& path,
                                  const OIIO::ImageSpec& s,
                                  const std::string& format,const ColourPlan& plan) {
    if(format!="png" || plan.route!=ColourPlan::Route::integer_linear ||
       s.format!=OIIO::TypeDesc::UINT16 || s.width<=0 || s.height<=0 ||
       std::uint64_t(s.width)*std::uint64_t(s.height)<png_scanline_min_pixels || s.get_int_attribute("Orientation",1)!=1)
        return false;
    unsigned char h[33]{};
    std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(h),sizeof(h)))return false;
    const unsigned char prefix[]{137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R'};
    auto be32=[](const unsigned char* p){return (std::uint32_t(p[0])<<24)|
        (std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];};
    return !std::memcmp(h,prefix,sizeof(prefix)) && be32(h+16)==std::uint32_t(s.width) &&
        be32(h+20)==std::uint32_t(s.height) && h[24]==16 &&
        ((h[25]==2 && s.nchannels==3) || (h[25]==6 && s.nchannels==4)) && h[26]==0 && h[27]==0 && h[28]==0;
}

// One forward pass, bounded native block plus one lazily allocated row. Read
// whole rows directly into the block; only a row crossing a block boundary
// needs scratch (including rows wider than the block). The transform calls
// retain EXACTLY the original global 1M-pixel boundaries and final tail.
// Reader failures return immediately; callers must never retry full decode.
// Scratch is replaceable in tests to verify lazy allocation without runtime
// counters or hooks in the product. It follows the vector empty/resize/data API.
template<class ReadRows,class Transform,class Scratch=std::vector<std::uint16_t>>
bool png_scanline_blocks(int width,int height,int channels,ReadRows read,Transform transform) {
    const auto count=std::size_t(width)*height;
    const auto capacity=std::min(count,colour_chunk_pixels);
    std::vector<std::uint16_t> block(capacity*channels);
    Scratch row;
    std::size_t used=0,base=0;
    auto flush=[&]() {transform(block.data(),base,used);base+=used;used=0;};
    for(int y=0;y<height;) {
        const auto rows=std::min((capacity-used)/std::size_t(width),std::size_t(height-y));
        if(rows) {
            if(!read(y,y+int(rows),block.data()+used*channels))return false;
            y+=int(rows);used+=rows*std::size_t(width);
            if(used==capacity)flush();
            continue;
        }
        if(row.empty())row.resize(std::size_t(width)*channels);
        if(!read(y,y+1,row.data()))return false;
        ++y;
        for(std::size_t x=0;x<std::size_t(width);) {
            const auto n=std::min(capacity-used,std::size_t(width)-x);
            std::memcpy(block.data()+used*channels,row.data()+x*channels,n*channels*sizeof(std::uint16_t));
            used+=n;x+=n;
            if(used==capacity)flush();
        }
    }
    if(used)flush();
    return true;
}
}
