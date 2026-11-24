#pragma once
#include <thumbgen/core.h>
#include <OpenImageIO/imageio.h>
#include "data_preview.h"
#include "selection_request.h"

namespace thumbgen::detail {
struct Candidate {
    std::string group, kind, reason;
    std::vector<int> channels;
};
std::vector<Candidate> classify(const OIIO::ImageSpec& spec);
std::vector<Candidate> classify(const OIIO::ImageSpec& spec,const std::vector<std::string>& crypto);
bool is_primary(const OIIO::ImageSpec& spec, const Candidate& candidate);
bool is_root_rgb(const OIIO::ImageSpec& spec, const Candidate& candidate);
bool is_auxiliary_aov(const OIIO::ImageSpec& spec, const Candidate& candidate);
Json candidate_json(const OIIO::ImageSpec& spec, const Candidate& candidate, int part);
Candidate data_candidate(const OIIO::ImageSpec& spec,const Candidate& candidate,const std::string& requested,const DataMapping& mapping);
}
