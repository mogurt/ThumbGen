#pragma once
#include <thumbgen/core.h>
namespace thumbgen::detail {
Json selection_request(const Job& job);
inline bool auto_cryptomatte_allowed(const Options& options) {
    return options.visualization=="auto" && options.channels.empty() && options.part<0 &&
        options.part_name.empty() && options.deep_mode=="reject";
}
}
