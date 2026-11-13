#pragma once

#include "build_info.h"
#include <OpenImageIO/version.h>
#include <nlohmann/json.hpp>

namespace thumbgen::detail {
// Handshake identity is build data, not a runtime plugin-capability query.
// Keep the exact public version/build types; core_build_identity tests this
// against Engine::capabilities() so future version edits cannot silently drift.
inline const nlohmann::json& immutable_build_identity() {
    static const nlohmann::json value{
        {"build", {{"revision", THUMBGEN_SOURCE_REVISION},
                   {"dirty", THUMBGEN_SOURCE_DIRTY},
                   {"compiler", THUMBGEN_COMPILER},
                   {"platform", THUMBGEN_PLATFORM},
                   {"architecture", THUMBGEN_ARCH}}},
        {"version", "0.1.0-dev"},
        {"oiio_version", OIIO_VERSION_STRING}
    };
    return value;
}
} // namespace thumbgen::detail
