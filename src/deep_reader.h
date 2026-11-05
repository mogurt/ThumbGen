#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace thumbgen::detail {
// Internal decoder contract. Input colours must already be associated and
// linear. Selection and OCIO policy belong to the caller, not this reader.
struct DeepChannels {
    std::array<std::string,3> colour;
    // Empty alpha explicitly means opaque for that component.
    std::array<std::string,3> alpha;
    std::string z,zback; // Empty zback means point samples.
};
struct DeepReadLimits {
    std::size_t bytes=512*1024*1024;
    std::size_t samples_per_pixel=4096;
    std::uint64_t samples_per_chunk=16*1024*1024,samples_per_image=64*1024*1024;
};
struct DeepImage {
    int x=0,y=0,width=0,height=0;
    // Interleaved linear R/G/B and three independent alpha components. The
    // caller must choose a background/ordinary-alpha policy before encoding.
    std::unique_ptr<float[]> pixels;
    std::size_t bytes=0,peak_accounted_bytes=0;
    std::uint64_t samples=0,chunks=0;
};
using DeepCheckpoint=std::function<void(const char* phase)>;
DeepImage read_deep_image(const std::filesystem::path& path,int part,const DeepChannels& channels,
                         DeepReadLimits limits={},DeepCheckpoint checkpoint={});
}
