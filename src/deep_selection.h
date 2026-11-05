#pragma once
#include "selection.h"
#include "deep_reader.h"

namespace thumbgen::detail {
// Metadata-only building blocks for public Deep integration. Never inspect
// sample values or infer opacity from the current frame.
std::vector<Candidate> deep_colour_candidates(const OIIO::ImageSpec& spec);
struct DeepSelection {
    DeepChannels channels;
    bool shared_alpha=false;
    // Exact names, not channel indices: suitable for sequence identity across
    // channel reorderings. Also records point/volume and scalar/colour alpha.
    Json identity() const;
};
DeepSelection deep_selection(const OIIO::ImageSpec& spec,const Candidate& colour);
}
