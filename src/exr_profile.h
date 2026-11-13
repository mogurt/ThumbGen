#pragma once
// Internal, opt-in diagnostics only. Disabled macros evaluate no arguments and
// create no clocks/JSON objects. These intervals overlap existing timers; they
// are NOT a partition of process wall time. All output is numeric timings_ms.
#include <cstddef>
namespace thumbgen::detail {
// Limit NOZERO to the observed large-buffer coverage (4k selected RGB: 96 MiB).
// Benefits were measured for cohorts, not proven per image at this boundary.
// This is not an optimal threshold or a thread policy. Compare checked element
// counts, not bytes obtained by a potentially overflowing multiplication.
inline constexpr std::size_t exr_nozero_min_bytes=96u*1024u*1024u;
inline constexpr std::size_t exr_nozero_min_samples=exr_nozero_min_bytes/sizeof(float);
}
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
#include <chrono>
#include <thumbgen/core.h>
namespace thumbgen::detail {
class ExrProfile {
    Json& record_;
    bool enabled_=false;
public:
    using Clock=std::chrono::steady_clock;
    explicit ExrProfile(Json& record):record_(record) {}
    void enable(bool value) {enabled_=value;}
    // Diagnostic route marker, not a duration; still a schema-compatible
    // nonnegative number inside timings_ms. Absent in non-profile builds.
    void mark(const char* key,double value) {if(enabled_)record_["timings_ms"][key]=value;}
    Clock::time_point tick() const {return enabled_?Clock::now():Clock::time_point{};}
    void add(const char* key,Clock::time_point start) {
        if(!enabled_)return;
        auto& value=record_["timings_ms"][key];
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        value=(value.is_number()?value.get<double>():0.0)+elapsed;
    }
};
}
#define TG_EXR_MARK(key,value) profile.mark(key,value)
#define TG_EXR_TICK(name) const auto name=profile.tick()
#define TG_EXR_TOCK(name,key) profile.add(key,name)
#define TG_EXR_PROFILE_PARAM , thumbgen::detail::ExrProfile& profile
#define TG_EXR_PROFILE_ARG , profile
#else
#define TG_EXR_MARK(key,value)
#define TG_EXR_TICK(name)
#define TG_EXR_TOCK(name,key)
#define TG_EXR_PROFILE_PARAM
#define TG_EXR_PROFILE_ARG
#endif
