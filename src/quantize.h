#pragma once
#include <cstddef>
#include <cstdint>

namespace thumbgen::detail {
// unorm8-half-up-v1: clamp [0,1], binary32 multiply by 255, round
// half upward. NaN/-Inf -> 0, +Inf -> 255. Independent of chunk tails.
void quantize_u8(const float* source,std::uint8_t* destination,std::size_t count) noexcept;
}
