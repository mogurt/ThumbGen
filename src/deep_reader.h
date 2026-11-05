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
    // Internal fault injection: reject this 1-based Core allocation (0 disables).
    std::size_t fail_core_allocation=0;
};
// Runtime identity of the audited Core implementation, never a build option.
bool bounded_deep_zstd_available() noexcept;
enum class DeepOutputMode {components,associated_rgba,opaque_rgba};
struct DeepReadOutput {
    DeepOutputMode mode=DeepOutputMode::components;
    // In the same linear space as the source. Used only by opaque_rgba.
    std::array<double,3> background{};
};
struct DeepImage {
    int x=0,y=0,width=0,height=0;
    int components=6;
    // Default: interleaved R/G/B/alphaR/alphaG/alphaB. Explicit RGBA modes use
    // four components. Associated RGBA requires one structural alpha mapping;
    // additive RGB at zero alpha remains associated data, not straight PNG.
    std::unique_ptr<float[]> pixels;
    std::size_t bytes=0,peak_accounted_bytes=0;
    // Internal diagnostic: calls to the request-scoped Core allocator.
    // Excludes our reserved output/plane/kernel arrays and process memory.
    std::size_t core_allocation_requests=0;
    std::uint64_t samples=0,chunks=0;
};
using DeepCheckpoint=std::function<void(const char* phase)>;
DeepImage read_deep_image(const std::filesystem::path& path,int part,const DeepChannels& channels,
                         DeepReadLimits limits={},DeepCheckpoint checkpoint={},DeepReadOutput output={});
}
