// Standalone internal diagnostics contract. Build twice with PROFILE=0 and 1.
#include "exr_profile.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main() {
    try {
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
        thumbgen::Json r={{"timings_ms",thumbgen::Json::object()}};
        thumbgen::detail::ExrProfile profile(r);
        TG_EXR_TICK(inactive);TG_EXR_TOCK(inactive,"exr_test_ms");
        TG_EXR_MARK("exr_nozero_hit",1.0);
        require(r["timings_ms"].empty(),"inactive route must not add fields");
        profile.enable(true);
        TG_EXR_TICK(first);TG_EXR_TOCK(first,"exr_test_ms");
        double before=r["timings_ms"]["exr_test_ms"].get<double>();
        TG_EXR_TICK(second);TG_EXR_TOCK(second,"exr_test_ms");
        double after=r["timings_ms"]["exr_test_ms"].get<double>();
        require(std::isfinite(before)&&before>=0&&after>=before,"nonnegative numeric accumulated interval");
        TG_EXR_MARK("exr_nozero_hit",1.0);
        require(r["timings_ms"]["exr_nozero_hit"].is_number() &&
                r["timings_ms"]["exr_nozero_hit"].get<double>()==1.0,"numeric route marker");
        TG_EXR_MARK("exr_nozero_hit",0.0);
        profile.enable(false);TG_EXR_MARK("exr_nozero_hit",1.0);
        require(r["timings_ms"]["exr_nozero_hit"].get<double>()==0.0,"marker replacement and inactive no-op");
        require(r.size()==1,"no top-level profile metadata");
#else
        // If disabled, even undefined expressions passed to macros disappear.
        TG_EXR_TICK(disabled);TG_EXR_TOCK(undefined_variable,undefined_key);
        TG_EXR_MARK(undefined_marker,undefined_value);
#endif
        std::cout<<"EXR profile contract passed\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
