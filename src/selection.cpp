#include "selection.h"
#include "files.h"
#include "cryptomatte.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace thumbgen::detail {
static std::string lower(std::string name) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    return name;
}
static bool data_name(const std::string& name) {
    static const std::set<std::string> tokens{"z","depth","n","normal","normals","p","position","worldposition",
        "uv","uvw","uvmap","motion","motionvector","velocity","vector","id","ids","objectid","materialid",
        "object_id","material_id","crypto","cryptomatte"};
    auto text=lower(name);
    if(tokens.count(text))return true;
    std::string token;
    for(size_t i=0;i<=text.size();++i) {
        if(i<text.size() && std::isalnum(static_cast<unsigned char>(text[i])))token+=text[i];
        else {
            if(tokens.count(token) || token.rfind("crypto",0)==0)return true;
            token.clear();
        }
    }
    return false;
}
std::vector<Candidate> classify(const OIIO::ImageSpec& spec) {
    return classify(spec,crypto_names(spec));
}
std::vector<Candidate> classify(const OIIO::ImageSpec& spec,const std::vector<std::string>& crypto) {
    std::map<std::string,std::vector<int>> groups;
    for(int i=0;i<spec.nchannels;++i) {
        const auto& name=spec.channelnames[i];auto dot=name.rfind('.');
        groups[dot==std::string::npos?"":name.substr(0,dot)].push_back(i);
    }
    auto component=[&](int c) {auto name=spec.channelnames[c];return lower(name.substr(name.rfind('.')==std::string::npos?0:name.rfind('.')+1));};
    auto channel=[&](const std::vector<int>& indices,const std::string& name) {
        int found=-1;
        for(int c:indices)if(component(c)==name){if(found!=-1)return -1;found=c;}
        return found;
    };
    const auto part_name=std::string(spec.get_string_attribute("oiio:subimagename"));
    const bool data_part=data_name(part_name) || crypto_group(crypto,part_name);
    std::vector<Candidate> result;
    for(const auto& entry:groups) {
        const auto& prefix=entry.first;const auto& indices=entry.second;
        bool data=data_part || data_name(prefix) || crypto_group(crypto,prefix);
        int r=channel(indices,"r"),g=channel(indices,"g"),b=channel(indices,"b");
        if(r<0 || g<0 || b<0)for(int c:indices)data=data || data_name(component(c));
        std::vector<int> selected;
        std::string kind="unknown",reason="unrecognized channel layout";
        if(r>=0 && g>=0 && b>=0) {
            selected={r,g,b};kind="colour";reason="complete RGB group";
            int a=channel(indices,"a");
            auto root=groups.find("");
            if(a<0 && !prefix.empty() && root!=groups.end())a=channel(root->second,"a");
            if(a>=0)selected.push_back(a);
        } else if(indices.size()==1 || indices.size()==2) {
            int grey=-1,alpha=channel(indices,"a");
            for(int c:indices)if(component(c)=="y" || component(c)=="gray" || component(c)=="grey" || component(c)=="luminance")grey=c;
            if(grey>=0 && (indices.size()==1 || (alpha>=0 && indices.size()==2))) {
                selected={grey,grey,grey};if(alpha>=0)selected.push_back(alpha);
                kind="grey";reason="recognized luminance channel";
            }
        }
        if(data){kind="data";reason="data semantic in part, group or channel name";selected=indices;}
        if(selected.empty())selected=indices;
        result.push_back({prefix.empty()?(kind=="grey"?"GRAY":"RGB"):prefix,kind,reason,selected});
        if(kind=="colour" || kind=="grey")for(int c:indices) {
            if(std::find(selected.begin(),selected.end(),c)!=selected.end() || component(c)=="a")continue;
            result.push_back({spec.channelnames[c],data_name(component(c))?"data":"unknown",
                "auxiliary channel outside colour group",{c}});
        }
    }
    if(spec.deep)for(auto& candidate:result){candidate.kind="deep";candidate.reason="Deep samples require flattening";}
    return result;
}
Json candidate_json(const OIIO::ImageSpec& spec,const Candidate& candidate,int part) {
    Json names=Json::array();for(int c:candidate.channels)names.push_back(spec.channelnames[c]);
    return {{"part",part},{"part_name",spec.get_string_attribute("oiio:subimagename")},
        {"group",candidate.group},{"kind",candidate.kind},{"reason",candidate.reason},{"channels",names},
        {"primary",is_primary(spec,candidate)}};
}
bool is_primary(const OIIO::ImageSpec& spec,const Candidate& candidate) {
    if(candidate.kind!="colour")return false;
    auto main_name=[](std::string name) {
        auto dot=name.rfind('.');if(dot!=std::string::npos)name=name.substr(dot+1);
        name=lower(name);return name=="beauty" || name=="combined" || name=="rgba";
    };
    return main_name(candidate.group) || (candidate.group=="RGB" && main_name(std::string(spec.get_string_attribute("oiio:subimagename"))));
}
Json selection_request(const Job& job) {
    const auto& o=job.options;
    Json request{{"part",o.part},{"part_name",o.part_name},{"channels",o.channels},{"strategy",o.selection_policy}};
    if(o.deep_mode!="reject")request["deep_mode"]=o.deep_mode;
    if(o.visualization=="auto")request["auto_preview"]=o.invalid_values;
    else if(o.visualization!="colour") {
        auto mapping=data_mapping(o.visualization,o.data_range,o.invalid_values);
        request["data"]={{"mode",o.visualization},{"range",mapping.mode==DataMode::id || mapping.mode==DataMode::cryptomatte?Json::array():Json::array({mapping.minimum,mapping.maximum})},
            {"invalid_values",o.invalid_values},{"policy","v1"}};
    }
    return request;
}
Candidate data_candidate(const OIIO::ImageSpec& spec,const Candidate& original,const std::string& requested,const DataMapping& mapping) {
    auto group=original;
    const auto semantic=lower(group.group+" "+std::string(spec.get_string_attribute("oiio:subimagename")));
    bool crypto=semantic.find("crypto")!=std::string::npos;
    for(const auto& attribute:spec.extra_attribs) {
        const std::string name=attribute.name().string();
        if(name.rfind("cryptomatte/",0)!=0 || name.size()<5 || name.substr(name.size()-5)!="/name")continue;
        const std::string prefix=spec.get_string_attribute(name);
        if(!prefix.empty() && group.group.rfind(prefix,0)==0 && std::all_of(group.group.begin()+prefix.size(),group.group.end(),[](unsigned char c){return c>='0' && c<='9';}))crypto=true;
    }
    if(crypto)throw FileFailure("cryptomatte_requires_visualization","Cryptomatte IDs/coverage require their dedicated preview");
    if(requested!=group.group) {
        auto match=std::find_if(group.channels.begin(),group.channels.end(),[&](int c){return spec.channelnames[c]==requested;});
        if(match==group.channels.end())throw FileFailure("channels_not_found","requested data channel does not exist");
        group.channels={*match};group.group=requested;
    }
    if(mapping.mode==DataMode::depth || mapping.mode==DataMode::id) {
        if(requested==original.group) {
            std::vector<int> unique;
            for(int c:group.channels) {
                auto name=spec.channelnames[c];name=lower(name.substr(name.rfind('.')==std::string::npos?0:name.rfind('.')+1));
                if(name!="a" && std::find(unique.begin(),unique.end(),c)==unique.end())unique.push_back(c);
            }
            group.channels=std::move(unique);
        }
        if(group.channels.size()!=1)throw FileFailure("visualization_layout_unsupported","depth/ID requires one explicit channel");
    } else {
        auto find=[&](const char* component) {
            int result=-1;
            for(int c:group.channels) {
                auto name=spec.channelnames[c];name=lower(name.substr(name.rfind('.')==std::string::npos?0:name.rfind('.')+1));
                if(name==component){if(result!=-1)return -1;result=c;}
            }
            return result;
        };
        std::vector<int> components;
        for(const auto& labels:std::vector<std::vector<const char*>>{{"x","y","z"},{"r","g","b"},{"u","v"}}) {
            if(mapping.mode==DataMode::id_rgb && std::string(labels[0])!="r")continue;
            std::vector<int> found;for(auto label:labels){auto c=find(label);if(c<0)break;found.push_back(c);}
            if(found.size()==3 || (mapping.mode==DataMode::vector && found.size()==2)) {components=std::move(found);break;}
        }
        if(components.empty())throw FileFailure("visualization_layout_unsupported",mapping.mode==DataMode::id_rgb?"RGB labels require a complete RGB group":"normal requires XYZ/RGB; vector requires XY/XYZ, RG/RGB or UV");
        group.channels=std::move(components);
    }
    group.kind="data";group.reason="explicit data visualization";return group;
}
}
