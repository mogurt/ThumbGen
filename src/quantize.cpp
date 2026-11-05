#include "quantize.h"
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define THUMBGEN_QUANTIZE_SSE2 1
#endif

namespace thumbgen::detail {
namespace {
std::uint8_t scalar(float value) noexcept {
    if(!(value>0.0f))return 0; // Also handles NaN and negative infinity.
    if(value>=1.0f)return 255;
    const float scaled=value*255.0f;
    const auto base=static_cast<unsigned>(scaled);
    // Adding 0.5f can itself round a value just below a boundary upward.
    // Compare the fractional part instead; all integers here are exact floats.
    return static_cast<std::uint8_t>(base+(scaled-static_cast<float>(base)>=0.5f));
}
}
void quantize_u8(const float* source,std::uint8_t* destination,std::size_t count) noexcept {
    std::size_t i=0;
#ifdef THUMBGEN_QUANTIZE_SSE2
    const auto zero=_mm_setzero_ps(),one=_mm_set1_ps(1.0f),scale=_mm_set1_ps(255.0f),half=_mm_set1_ps(0.5f);
    const auto increment=_mm_set1_epi32(1);
    for(;count-i>=4;i+=4) {
        // MAX's second operand supplies zero for NaN; both infinities clamp.
        const auto value=_mm_min_ps(_mm_max_ps(_mm_loadu_ps(source+i),zero),one);
        const auto scaled=_mm_mul_ps(value,scale);
        const auto base=_mm_cvttps_epi32(scaled);
        const auto fraction=_mm_sub_ps(scaled,_mm_cvtepi32_ps(base));
        const auto rounded=_mm_add_epi32(base,_mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(fraction,half)),increment));
        const auto words=_mm_packs_epi32(rounded,rounded);
        const auto packed=_mm_packus_epi16(words,words);
        const auto bytes=_mm_cvtsi128_si32(packed);
        std::memcpy(destination+i,&bytes,4);
    }
#endif
    for(;i<count;++i)destination[i]=scalar(source[i]);
}
void quantize_rgba8(const float* source,std::uint8_t* destination,std::size_t pixels) noexcept {
    quantize_u8(source,destination,pixels*4);
    for(std::size_t i=0;i<pixels;++i,destination+=4)
        if(destination[3]==0)destination[0]=destination[1]=destination[2]=0;
}
}
