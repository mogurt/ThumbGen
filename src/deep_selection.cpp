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
        // Scalar alpha selection belongs to deep_selection(), which also
        // considers the more expressive AR/AG/AB mapping in the same namespace.
        candidate.channels.resize(3);
        for(auto& c:candidate.channels)c=original.at(std::size_t(c));
        candidate.reason="Deep associated colour; explicit metadata mapping required";
    }
    return candidates;
}
Json DeepSelection::identity() const {
    return {{"policy","deep-mapping-v1"},{"colour",channels.colour},{"alpha",channels.alpha},
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
    auto alpha=[&](const std::string& space) {
        std::array<std::string,3> values{find(space,"ar"),find(space,"ag"),find(space,"ab")};
        const auto used=std::count_if(values.begin(),values.end(),[](const auto& value){return !value.empty();});
        if(used && used!=3)throw DeepFailure("deep_channel_mapping_invalid","Deep per-component alpha must have all AR/AG/AB channels");
        // Validate scalar ambiguity too, even if the complete triple takes precedence.
        auto scalar=find(space,"a");
        if(used==3)return values;
        return std::array<std::string,3>{scalar,scalar,scalar};
    };
    result.channels.alpha=alpha(group);
    if(result.channels.alpha[0].empty() && !group.empty())result.channels.alpha=alpha("");
    if(result.channels.alpha[0].empty())
        throw DeepFailure("deep_channel_mapping_invalid","Deep colour has no explicit A or complete AR/AG/AB; implicit opaque samples are not inferred");
    result.shared_alpha=result.channels.alpha[0]==result.channels.alpha[1] && result.channels.alpha[0]==result.channels.alpha[2];
    auto depth_group=group;
    result.channels.z=find(depth_group,"z");
    if(result.channels.z.empty() && !group.empty()){depth_group.clear();result.channels.z=find(depth_group,"z");}
    if(result.channels.z.empty())throw DeepFailure("deep_channel_mapping_invalid","Deep colour requires a Z channel");
    // Never combine local Z with a different namespace's Zback.
    result.channels.zback=find(depth_group,"zback");
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
