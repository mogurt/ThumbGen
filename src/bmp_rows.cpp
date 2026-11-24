#include "other_rows_internal.h"

namespace thumbgen::detail::other_rows {
OtherRowsProbe bmp(Source& source) {
    std::array<Byte,54> header{};
    source.read(0,header.data(),18,"truncated BMP header");
    const auto dib=le32(header.data()+14);
    const bool core=dib==12;
    require(core || dib==40 || dib==52 || dib==56 || dib==108 || dib==124,
        "unsupported BMP header length");
    source.read(18,header.data()+18,core?8:36,"truncated BMP layout");
    const auto width=core?le16(header.data()+18):le32(header.data()+18);
    const auto encoded_height=core?le16(header.data()+20):le32(header.data()+22);
    const auto height=!core && encoded_height>0x7fffffffU?0U-encoded_height:encoded_height;
    require(width<=0x7fffffffU && height<=0x7fffffffU,"invalid BMP dimensions");
    const auto pixels=dimensions(width,height,65536,"BMP dimensions exceed supported limits");
    const auto planes=le16(header.data()+(core?22:26)),bits=le16(header.data()+(core?24:28));
    require(planes==1 && (bits==1 || bits==4 || bits==8 || bits==16 || bits==24 || bits==32),
        "invalid BMP planes or bit depth");
    const auto compression=core?0:le32(header.data()+30);
    const auto offset=le32(header.data()+10);
    require(offset>=14+dib && offset<=source.size,"invalid BMP pixel offset");
    const auto stride=(std::uint64_t(width)*bits+31)/32*4;
    const auto codec=plugin_bytes+2*stride;
    if(compression || (bits!=24 && bits!=32)) {
        const auto retained=compression==1 || compression==2?pixels:0;
        return fallback(source,"bmp",pixels,codec+retained,retained);
    }
    source.extent(offset,stride*height,"truncated BMP image data");
    reserve(codec,source.budget,"BMP header and decode rows exceed decode budget");
    auto spec=metadata(source,"bmp");
    return supported(source,"bmp",std::move(spec),0,codec);
}
}
