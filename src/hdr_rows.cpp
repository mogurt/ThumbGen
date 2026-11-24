#include "other_rows_internal.h"

namespace thumbgen::detail::other_rows {
OtherRowsProbe hdr(Source& source) {
    // The plugin reads at most 100 header lines in fixed 128-byte buffers.
    // One read at a time allocates 4 bytes/pixel of RGBE scratch. The
    // sequential reader also keeps row offsets in a growing vector.
    reserve(plugin_bytes,source.budget,"HDR header exceeds decode budget");
    auto spec=metadata(source,"hdr");
    const auto bytes=plugin_bytes+4*std::uint64_t(spec.width)+
        4*sizeof(std::int64_t)*(std::uint64_t(spec.height)+1);
    return supported(source,"hdr",std::move(spec),0,bytes);
}
}
