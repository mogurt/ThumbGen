#pragma once
#include "data_image.h"
#include <thumbgen/core.h>
#include <map>
namespace thumbgen::detail {
struct CryptoLayer {std::string name;int part;std::vector<int> channels;};
struct CryptoLayout {
    std::string name,metadata_key;
    std::vector<CryptoLayer> layers;
    std::vector<std::string> channel_names;
    std::map<int,OIIO::ImageSpec> parts;
    bool embedded_manifest=false,sidecar_manifest=false;
    Json info() const;
};
// Lightweight semantic classification; malformed optional crypto metadata must
// not block an unrelated Beauty conversion.
std::vector<std::string> crypto_names(const OIIO::ImageSpec& spec);
bool is_object_crypto_family(const std::string& name);
bool crypto_group(const std::vector<std::string>& names,const std::string& group);
CryptoLayout crypto_layout(std::vector<OIIO::ImageSpec> specs,const std::string& family,int selected_part=-1);
std::size_t crypto_source_pixel_bytes(const CryptoLayout& layout);
// Read one rank layer at a time. IDs remain opaque FLOAT bits; only coverage
// is interpreted numerically. No manifest or external sidecar is needed/read.
DataImage read_crypto_image(OIIO::ImageInput& input,const CryptoLayout& layout,InvalidData invalid,std::size_t limit,int threads);
}
