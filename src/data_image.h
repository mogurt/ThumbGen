#pragma once
#include "data_preview.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
namespace thumbgen::detail {
struct DataImage {
    OIIO::ImageBuf image;
    DataStats statistics;
    std::size_t decoded_bytes=0;
    double decode_ms=0,mapping_ms=0;
};
std::size_t data_sample_bytes(const OIIO::ImageSpec& spec,const std::vector<int>& channels);
DataImage read_data_image(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const std::vector<int>& channels,const DataMapping& mapping,std::size_t limit,int threads);
}
