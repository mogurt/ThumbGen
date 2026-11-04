#pragma once
#include <cstdint>
namespace thumbgen::detail {
inline void id_palette(std::uint32_t id,float* rgba) noexcept {
    std::uint32_t h=id*2654435761u;
    h^=(h<<13)|(h>>19);h^=h>>17;
    for(unsigned c=0;c<3;++c)rgba[c]=id?float(64+((h>>(c*8))%192u))/255.0f:0.0f;
    rgba[3]=1;
}
}
