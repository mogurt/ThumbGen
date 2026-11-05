#pragma once
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace thumbgen::detail {
inline constexpr std::size_t icc_profile_limit=4*1024*1024;
inline constexpr std::size_t cms_memory_limit=32*1024*1024;
inline constexpr std::size_t cms_jpeg_limit=8*1024*1024;
inline constexpr std::size_t cms_reservation=4*icc_profile_limit+cms_memory_limit+cms_jpeg_limit+128*1024;
enum class CmsModel {rgb,grey,cmyk};
struct CmsFailure:std::runtime_error {using std::runtime_error::runtime_error;};
struct CmsStats {std::size_t live=0,peak=0,allocations=0,denied=0;};
// No global allocator/processor/cache. Stats must outlive this request object.
class CmsInput {
public:
    CmsInput(const void* profile,std::size_t bytes,CmsModel model,CmsStats& stats,std::size_t limit=cms_memory_limit);
    ~CmsInput();
    void apply(float* pixels,std::size_t count,int channels,bool associated);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
