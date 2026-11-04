#pragma once
#include "data_image.h"
#include <thumbgen/core.h>
#include <optional>
namespace thumbgen::detail {
struct CryptoLayer {std::string name;std::vector<int> channels;};
struct CryptoLayout {
    std::string name,metadata_key;
    std::vector<CryptoLayer> layers;
    std::vector<int> channels;
    bool embedded_manifest=false,sidecar_manifest=false;
    Json info() const;
};
// Lightweight semantic classification; malformed optional crypto metadata must
// not block an unrelated Beauty conversion.
std::vector<std::string> crypto_names(const OIIO::ImageSpec& spec);
bool crypto_group(const std::vector<std::string>& names,const std::string& group);
std::optional<CryptoLayout> crypto_layout(const OIIO::ImageSpec& spec,const std::string& family);
std::size_t crypto_source_pixel_bytes(const OIIO::ImageSpec& spec,const CryptoLayout& layout);
// Read one rank layer at a time. IDs remain opaque FLOAT bits; only coverage
// is interpreted numerically. No manifest or external sidecar is needed/read.
DataImage read_crypto_image(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const CryptoLayout& layout,InvalidData invalid,std::size_t limit,int threads);
}
