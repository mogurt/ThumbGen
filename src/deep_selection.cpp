#include "deep_selection.h"
#include "deep_flatten.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace thumbgen::detail {
namespace {
std::string lower(std::string text) {
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});
    return text;
}
std::string component(const std::string& text) {
    auto dot=text.rfind('.');return lower(text.substr(dot==std::string::npos?0:dot+1));
}
std::string prefix(const std::string& text) {
    auto dot=text.rfind('.');return dot==std::string::npos?std::string{}:text.substr(0,dot);
}
bool alpha_group(const std::string& name) {
    const auto last=component(name);
    return last=="a" || last=="alpha" || last=="opacity" || last=="transmission" || last=="transmittance";
}
void validate_spec(const OIIO::ImageSpec& spec) {
    if(!spec.deep || spec.nchannels<=0 || spec.nchannels>4096 || spec.channelnames.size()!=std::size_t(spec.nchannels) ||
       (!spec.channelformats.empty() && spec.channelformats.size()!=std::size_t(spec.nchannels)))
        throw DeepFailure("deep_channel_mapping_invalid","Deep channel metadata is incomplete or exceeds the supported channel limit");
}
}
std::vector<Candidate> deep_colour_candidates(const OIIO::ImageSpec& spec) {
    validate_spec(spec);
    auto view=spec;view.deep=false;view.channelnames.clear();view.channelformats.clear();
    view.alpha_channel=view.z_channel=-1;
    std::vector<int> original;
    for(int c=0;c<spec.nchannels;++c) {
        const auto name=component(spec.channelnames[c]);
        // These are support planes, not evidence that root luminance is data.
        if(name=="z" || name=="zback" || name=="ar" || name=="ag" || name=="ab")continue;
        original.push_back(c);view.channelnames.push_back(spec.channelnames[c]);
    }
    view.nchannels=int(original.size());
    auto candidates=classify(view);
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const auto& c){
        return (c.kind!="colour" && c.kind!="grey") || alpha_group(c.group) ||
            alpha_group(std::string(spec.get_string_attribute("oiio:subimagename")));
    }),candidates.end());
    for(auto& candidate:candidates) {
        // Alpha selection belongs to deep_selection(), which resolves each
        // colour separately through its own and enclosing layers.
        candidate.channels.resize(3);
        for(auto& c:candidate.channels)c=original.at(std::size_t(c));
        candidate.reason="Deep associated colour; explicit metadata mapping required";
    }
    return candidates;
}
Json DeepSelection::identity() const {
    return {{"policy","deep-mapping-v2"},{"colour",channels.colour},{"alpha",channels.alpha},
        {"z",channels.z},{"zback",channels.zback},{"shared_alpha",shared_alpha}};
}
DeepSelection deep_selection(const OIIO::ImageSpec& spec,const Candidate& colour) {
    validate_spec(spec);
    if(!spec.deep || (colour.kind!="colour" && colour.kind!="grey") || colour.channels.size()<3)
        throw DeepFailure("deep_channel_mapping_invalid","Deep selection requires a colour or luminance candidate");
    DeepSelection result;
    std::string group;
    for(int c=0;c<3;++c) {
        int index=colour.channels[c];
        if(index<0 || index>=spec.nchannels)throw DeepFailure("deep_channel_mapping_invalid","Deep colour index is out of range");
        result.channels.colour[c]=spec.channelnames[index];
        auto current=prefix(spec.channelnames[index]);
        if(c && current!=group)throw DeepFailure("deep_channel_mapping_invalid","Deep colour components must share a namespace");
        group=current;
    }
    // Case-insensitive components within an exact, case-sensitive namespace.
    // Duplicate case variants are ambiguous even if their pixel values match.
    auto find=[&](const std::string& space,const std::string& suffix) {
        std::string found;
        for(const auto& name:spec.channelnames)if(prefix(name)==space && component(name)==suffix) {
            if(!found.empty())throw DeepFailure("deep_channel_mapping_invalid","Deep support channel has ambiguous case variants");
            found=name;
        }
        return found;
    };
    // Resolve each colour independently. A closer A wins over a farther AR/AG/AB;
    // partial triples are valid when the other components can find their own A.
    for(int c=0;c<3;++c) {
        const auto base=component(result.channels.colour[c]);
        const auto matching=base=="r"?"ar":base=="g"?"ag":base=="b"?"ab":"";
        auto space=group;
        for(;;) {
            auto alpha=matching[0]?find(space,matching):std::string{};
            if(alpha.empty())alpha=find(space,"a");
            if(!alpha.empty()){result.channels.alpha[c]=std::move(alpha);break;}
            if(space.empty())throw DeepFailure("deep_channel_mapping_invalid","Deep colour has no matching alpha in its layer or enclosing layers");
            space=prefix(space);
        }
    }
    result.shared_alpha=result.channels.alpha[0]==result.channels.alpha[1] && result.channels.alpha[0]==result.channels.alpha[2];
    // OpenEXR defines positions for all layers using the base layer only.
    // Layer-local *.Z / *.ZBack are auxiliary values, not sample positions.
    result.channels.z=find("","z");
    if(result.channels.z.empty())throw DeepFailure("deep_channel_mapping_invalid","Deep colour requires base-layer Z; layer-local depths are auxiliary channels");
    result.channels.zback=find("","zback");
    std::set<std::string> names(result.channels.colour.begin(),result.channels.colour.end());
    names.insert(result.channels.alpha.begin(),result.channels.alpha.end());names.insert(result.channels.z);
    if(!result.channels.zback.empty())names.insert(result.channels.zback);
    for(const auto& name:names) {
        auto found=std::find(spec.channelnames.begin(),spec.channelnames.end(),name);
        if(found==spec.channelnames.end() || std::count(spec.channelnames.begin(),spec.channelnames.end(),name)!=1)
            throw DeepFailure("deep_channel_mapping_invalid","Deep selected channel name is missing or duplicated");
        const auto type=spec.channelformat(int(found-spec.channelnames.begin()));
        if(type!=OIIO::TypeDesc::HALF && type!=OIIO::TypeDesc::FLOAT)
            throw DeepFailure("deep_channel_mapping_invalid","Deep selected colour, alpha and depth must use HALF or FLOAT");
    }
    return result;
}
}
