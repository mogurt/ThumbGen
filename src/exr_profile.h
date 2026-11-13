#pragma once
// Internal, opt-in diagnostics only. Disabled macros evaluate no arguments and
// create no clocks/JSON objects. These intervals overlap existing timers; they
// are NOT a partition of process wall time. All output is numeric timings_ms.
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
    Clock::time_point tick() const {return enabled_?Clock::now():Clock::time_point{};}
    void add(const char* key,Clock::time_point start) {
        if(!enabled_)return;
        auto& value=record_["timings_ms"][key];
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        value=(value.is_number()?value.get<double>():0.0)+elapsed;
    }
};
}
#define TG_EXR_TICK(name) const auto name=profile.tick()
#define TG_EXR_TOCK(name,key) profile.add(key,name)
#define TG_EXR_PROFILE_PARAM , thumbgen::detail::ExrProfile& profile
#define TG_EXR_PROFILE_ARG , profile
#else
#define TG_EXR_TICK(name)
#define TG_EXR_TOCK(name,key)
#define TG_EXR_PROFILE_PARAM
#define TG_EXR_PROFILE_ARG
#endif
