#pragma once
#include "data_preview.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
namespace thumbgen::detail {
struct CategoricalCoordinates {
    // Source-local X/Y coordinates. Transposed orientations index X by output Y.
    std::vector<int> x,y;
    bool transposed=false;
};
CategoricalCoordinates categorical_coordinates(const OIIO::ImageSpec& spec,int width,int height);
struct DataImage {
    OIIO::ImageBuf image;
    DataStats statistics;
    std::size_t decoded_bytes=0;
    double decode_ms=0,mapping_ms=0;
    // Categorical modes retain native samples and allocate RGBA only at output size.
    std::vector<unsigned char> native;
    OIIO::ImageSpec source_spec;
    std::vector<std::size_t> offsets,strides;
    std::vector<OIIO::TypeDesc> types;
    DataMapping mapping;
    std::vector<int> sampled_x,sampled_y;
    bool sampled=false;
};
DataImage read_data_image(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const std::vector<int>& channels,const DataMapping& mapping,std::size_t limit,int threads);
OIIO::ImageBuf categorical_preview(const DataImage& image,int width,int height,std::size_t limit,int threads);
}
