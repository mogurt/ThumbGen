#include "other_rows_internal.h"

namespace thumbgen::detail::other_rows {
OtherRowsProbe dpx(Source& source) {
    constexpr std::uint64_t header_size=2048;
    std::array<Byte,header_size> header{};
    source.read(0,header.data(),header.size(),"truncated DPX header");
    const bool little=is(header.data(),"XPDS");
    const auto u16=[&](int p){return little?le16(header.data()+p):unsigned(header[p])*256+header[p+1];};
    const auto u32=[&](int p){
        return little?le32(header.data()+p):std::uint32_t(u16(p))*65536+u16(p+2);
    };
    const auto orientation=u16(768),elements=u16(770);
    const auto width=u32(orientation>=4 && orientation<=7?776:772);
    const auto height=u32(orientation>=4 && orientation<=7?772:776);
    const auto pixels=dimensions(width,height,65536,"DPX dimensions exceed supported limits");
    require(elements>0 && elements<=8,"invalid DPX element count");
    const auto user=u32(32)==std::uint32_t(-1)?0:u32(32);
    source.extent(header_size,user,"truncated DPX user metadata");
    reserve(user,metadata_limit,"DPX user metadata exceeds limit");
    const auto codec=plugin_bytes+metadata_copies*user+16*std::uint64_t(width);
    reserve(codec,source.budget,"DPX header and codec scratch exceed decode budget");

    const unsigned bits=header[803],packing=u16(804),encoding=u16(806);
    const auto row_padding=u32(812);
    const bool rgb=elements==1 && !u32(780) && header[800]==50 && orientation<=7 &&
        encoding==0 && packing<=2 && (row_padding==0 || row_padding==std::uint32_t(-1));
    if(!rgb || (bits!=8 && bits!=10 && bits!=12 && bits!=16)) {
        // Complete-image reads may use an intermediate component conversion
        // buffer or unpacker. Bound all elements before invoking the plugin.
        const auto scratch=std::uint64_t(elements)*pixels*8*8;
        reserve(source.size,std::numeric_limits<std::int32_t>::max(),"DPX source exceeds decoder offset range");
        return fallback(source,"dpx",pixels,codec+scratch);
    }

    std::uint64_t stride=std::uint64_t(width)*(bits==8?3:6);
    if(bits==10 && packing!=0)stride=std::uint64_t(width)*4;
    else if((bits==10 || bits==12) && packing==0)stride=(std::uint64_t(width)*3*bits+31)/32*4;
    const auto offset=u32(808);
    require(offset>=header_size+user,"invalid DPX data offset");
    source.extent(offset,stride*height,"truncated DPX image data");
    // libdpx's element stream uses a signed long for offsets on Windows.
    reserve(offset+stride*height,std::numeric_limits<std::int32_t>::max(),"DPX image data exceeds decoder offset range");
    auto spec=metadata(source,"dpx");
    return supported(source,"dpx",std::move(spec),0,codec);
}
}
