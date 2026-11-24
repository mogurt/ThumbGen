#include "other_rows_internal.h"

namespace thumbgen::detail::other_rows {
OtherRowsProbe targa(Source& source) {
    std::array<Byte,18> header{};
    source.read(0,header.data(),header.size(),"truncated TGA header");
    const auto width=le16(header.data()+12),height=le16(header.data()+14);
    const auto pixels=dimensions(width,height,65535,"invalid TGA dimensions");
    const auto type=header[2],bits=header[16],flags=header[17];
    if(header[1] || type!=2 || (bits!=24 && bits!=32) || (flags&0xc0)) {
        // The plugin allocates one native frame of at most four channels and
        // a palette of at most 65535 four-byte entries (included in the MiB).
        return fallback(source,"targa",pixels,plugin_bytes+4*pixels);
    }
    const auto start=header.size()+header[0];
    const auto packed=pixels*(bits/8);
    source.extent(start,packed,"truncated TGA image data");
    if(source.size>=26) {
        std::array<Byte,26> footer{};
        const auto end=source.size-footer.size();
        source.read(end,footer.data(),footer.size(),"truncated TGA footer");
        if(is(footer.data()+8,"TRUEVISION-XFILE.",17)) {
            auto pixel_end=end;
            for(auto offset:{le32(footer.data()),le32(footer.data()+4)}) {
                if(!offset)continue;
                require(offset<=end,"invalid TGA extension or developer offset");
                pixel_end=std::min(pixel_end,std::uint64_t(offset));
            }
            require(start+packed<=pixel_end,"truncated TGA image data before metadata");
        }
    }
    reserve(plugin_bytes+packed,source.budget,"TGA native frame exceeds decode budget");
    auto spec=metadata(source,"targa");
    return supported(source,"targa",std::move(spec),packed,plugin_bytes);
}
}
