#pragma once
#include <OpenImageIO/imageio.h>
#include <string>
#include <limits>

namespace thumbgen::detail {
// Container presence catches profiles discarded by a decoder (e.g. incomplete
// JPEG ICC marker sequences and invalid PNG iCCP chunks). No profile is parsed.
bool container_has_icc(const std::string& filename,std::size_t limit=std::numeric_limits<std::size_t>::max());
bool container_separated_colour(const std::string& filename);
bool separated_colour(const OIIO::ImageSpec& spec);
}
