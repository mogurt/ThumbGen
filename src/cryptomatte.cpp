#include "cryptomatte.h"
#include "files.h"
#include "id_palette.h"
#include "selected_input.h"
#include <OpenImageIO/half.h>
#include <OpenImageIO/parallel.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <set>
#include <tuple>

namespace thumbgen::detail {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
bool name_attribute(const std::string& name) {
    std::size_t prefix=name.rfind("cryptomatte/",0)==0?12:name.rfind("exr/cryptomatte/",0)==0?16:0;
    return prefix && name.size()>prefix+5 && name.substr(name.size()-5)=="/name" &&
        name.substr(prefix,name.size()-prefix-5).find('/')==std::string::npos;
}
bool digits(const std::string& text){return !text.empty() && std::all_of(text.begin(),text.end(),[](unsigned char c){return c>='0' && c<='9';});}
std::string lower(std::string text){for(auto& c:text)if(c>='A' && c<='Z')c=char(c-'A'+'a');return text;}
template<class T> T value(const unsigned char* source){T result;std::memcpy(&result,source,sizeof(result));return result;}
float coverage(const unsigned char* source,bool is_half){return is_half?float(value<half>(source)):value<float>(source);}
}
std::vector<std::string> crypto_names(const OIIO::ImageSpec& spec) {
    std::set<std::string> names;
    for(const auto& attribute:spec.extra_attribs)if(name_attribute(attribute.name().string()) && attribute.type()==OIIO::TypeDesc::STRING) {
        auto name=std::string(spec.get_string_attribute(attribute.name()));if(!name.empty())names.insert(name);
    }
    return {names.begin(),names.end()};
}
bool crypto_group(const std::vector<std::string>& names,const std::string& group) {
    return std::any_of(names.begin(),names.end(),[&](const auto& name){return group==name || (group.rfind(name,0)==0 && digits(group.substr(name.size())));});
}
Json CryptoLayout::info() const {
    Json names=Json::array();for(const auto& layer:layers)names.push_back(layer.name);
    return {{"name",name},{"metadata_key",metadata_key},{"hash","MurmurHash3_32"},{"conversion","uint32_to_float32"},
        {"layers",names},{"ranks",channel_names.size()/2},{"manifest",{{"embedded_attribute",embedded_manifest},{"sidecar_attribute",sidecar_manifest},{"used",false}}}};
}
CryptoLayout crypto_layout(std::vector<OIIO::ImageSpec> specs,const std::string& family,int selected_part) {
    struct Group {std::string name;int part;std::map<int,int> components;};
    std::map<unsigned,Group> groups;
    std::set<std::string> expected_layers;
    std::set<int> rank_parts;
    CryptoLayout result;result.name=family;
    auto add_metadata=[&](const OIIO::ImageSpec& spec) {
        for(const auto& attribute:spec.extra_attribs) {
            const auto name=attribute.name().string();
            if(!name_attribute(name) || attribute.type()!=OIIO::TypeDesc::STRING || spec.get_string_attribute(attribute.name())!=family)continue;
            const auto key=name.substr(0,name.size()-5);
            if(!result.metadata_key.empty() && result.metadata_key!=key)
                throw FileFailure("cryptomatte_metadata_invalid","ambiguous Cryptomatte metadata for family");
            result.metadata_key=key;
            const auto* hash=spec.find_attribute(key+"/hash");
            const auto* conversion=spec.find_attribute(key+"/conversion");
            if(!hash || !conversion || hash->type()!=OIIO::TypeDesc::STRING || conversion->type()!=OIIO::TypeDesc::STRING)
                throw FileFailure("cryptomatte_metadata_invalid","missing or non-string Cryptomatte metadata");
            if(spec.get_string_attribute(key+"/hash")!="MurmurHash3_32" || spec.get_string_attribute(key+"/conversion")!="uint32_to_float32")
                throw FileFailure("cryptomatte_metadata_invalid","unsupported Cryptomatte hash/conversion");
            result.embedded_manifest|=spec.find_attribute(key+"/manifest")!=nullptr;
            result.sidecar_manifest|=spec.find_attribute(key+"/manif_file")!=nullptr;
        }
    };
    for(int part=0;part<int(specs.size());++part) {
        const auto& spec=specs[part];
        const auto part_name=std::string(spec.get_string_attribute("oiio:subimagename"));
        if(part_name!=family && crypto_group({family},part_name))expected_layers.insert(part_name);
        for(int c=0;c<spec.nchannels;++c) {
            const auto& channel=spec.channelnames[c];
            auto dot=channel.rfind('.');if(dot==std::string::npos)continue;
            auto name=channel.substr(0,dot);
            if(name.rfind(family,0)!=0 || !digits(name.substr(family.size())))continue;
            // A selected part may repeat a complete family, but it must not omit any numbered layers.
            if(selected_part>=0 && part!=selected_part){expected_layers.insert(std::move(name));continue;}
            rank_parts.insert(part);
            unsigned rank=0;
            for(char digit:name.substr(family.size())) {
                if(rank>(std::numeric_limits<unsigned>::max()-unsigned(digit-'0'))/10)throw FileFailure("cryptomatte_layout_unsupported","Cryptomatte layer index overflows");
                rank=rank*10+unsigned(digit-'0');
            }
            auto& group=groups.try_emplace(rank,Group{name,part,{}}).first->second;
            if(group.name!=name)
                throw FileFailure("cryptomatte_layout_unsupported","duplicate Cryptomatte layer index");
            if(group.part!=part)
                throw FileFailure("ambiguous_parts","multiple parts contain the same Cryptomatte layer; select a part with the complete family");
            auto component=lower(channel.substr(dot+1));int index=-1;
            if(component=="r" || component=="red")index=0;else if(component=="g" || component=="green")index=1;
            else if(component=="b" || component=="blue")index=2;else if(component=="a" || component=="alpha")index=3;
            if(index>=0 && !group.components.emplace(index,c).second)
                throw FileFailure("cryptomatte_layout_unsupported","duplicate Cryptomatte channel alias");
        }
    }
    // Unrelated beauty parts can carry incomplete copies of the family metadata.
    for(int part:rank_parts)add_metadata(specs[part]);
    if(result.metadata_key.empty())
        for(const auto& spec:specs)add_metadata(spec);
    if(result.metadata_key.empty())throw FileFailure("cryptomatte_metadata_invalid","requested Cryptomatte family requires metadata");
    if(groups.empty())throw FileFailure("cryptomatte_layout_unsupported","requested Cryptomatte family has no numbered layers");
    const auto& reference=specs[groups.begin()->second.part];
    auto windows=[](const OIIO::ImageSpec& s) {
        return std::tie(s.x,s.y,s.z,s.width,s.height,s.depth,s.full_x,s.full_y,s.full_z,s.full_width,s.full_height,s.full_depth);
    };
    const auto orientation=reference.get_int_attribute("Orientation",1);
    unsigned expected=0;
    for(const auto& entry:groups) {
        const auto& group=entry.second;
        if(entry.first!=expected++)throw FileFailure("cryptomatte_layout_unsupported","Cryptomatte layers must start at zero without gaps");
        expected_layers.erase(group.name);
        const auto& spec=specs[group.part];
        if(spec.deep)throw FileFailure("deep_requires_flattening","Cryptomatte overview requires flat ranks");
        if(windows(spec)!=windows(reference) || spec.get_int_attribute("Orientation",1)!=orientation)
            throw FileFailure("cryptomatte_layout_unsupported","Cryptomatte parts must have matching data and display windows and orientation");
        const auto& components=group.components;
        bool second=components.count(2) || components.count(3);
        if(!components.count(0) || !components.count(1) || (second && (!components.count(2) || !components.count(3))) ||
            (!second && entry.first!=groups.rbegin()->first))throw FileFailure("cryptomatte_layout_unsupported","incomplete Cryptomatte ID/coverage pair");
        CryptoLayer layer{group.name,group.part,{}};
        for(int index=0;index<(second?4:2);++index) {
            int channel=components.at(index);auto type=spec.channelformat(channel);
            if((index%2==0 && type!=OIIO::TypeDesc::FLOAT) || (index%2==1 && type!=OIIO::TypeDesc::FLOAT && type!=OIIO::TypeDesc::HALF))
                throw FileFailure("data_type_unsupported","Cryptomatte IDs require FLOAT; coverage requires FLOAT or HALF");
            layer.channels.push_back(channel);result.channel_names.push_back(spec.channelnames[channel]);
        }
        result.layers.push_back(std::move(layer));
    }
    if(!expected_layers.empty())
        throw FileFailure("cryptomatte_layout_unsupported","selected Cryptomatte layers do not cover the complete family");
    for(int part:rank_parts)result.parts.emplace(part,std::move(specs[part]));
    return result;
}
std::size_t crypto_source_pixel_bytes(const CryptoLayout& layout) {
    std::size_t maximum=0;
    for(const auto& layer:layout.layers)
        maximum=std::max(maximum,channel_plan(layout.parts.at(layer.part),layer.channels,true).pixel_bytes);
    return maximum;
}
DataImage read_crypto_image(OIIO::ImageInput& input,const CryptoLayout& layout,InvalidData invalid,std::size_t limit,int threads) {
    const auto& spec=layout.parts.at(layout.layers.front().part);
    if(spec.width<1 || spec.height<1 || spec.depth!=1)throw FileFailure("invalid_dimensions","invalid Cryptomatte image");
    const auto pixels=std::size_t(spec.width)*spec.height,source_bytes=crypto_source_pixel_bytes(layout);
    if(!source_bytes || pixels>limit/source_bytes)throw FileFailure("decode_limit_exceeded","Cryptomatte rank layer exceeds source limit");
    if(pixels>limit/(4*sizeof(double)))throw FileFailure("canvas_limit_exceeded","Cryptomatte accumulation buffer exceeds limit");
    DataImage result;result.decoded_bytes=pixels*source_bytes;
    std::vector<double> sums(pixels*4,0);std::vector<unsigned char> bad(pixels,0);
    // Read all contiguous ranks together in bounded scanline strips. This
    // avoids decompressing the same EXR chunks once per rank layer, while
    // keeping raw storage within the original single-layer allocation.
    const auto compression=spec.get_string_attribute("compression");
    // Concurrent strip scheduling regressed measured two-thread workers.
    // Keep those and unmeasured codecs on the original whole-layer reader.
    std::vector<int> combined_channels;
    bool strips=false;
    if(layout.parts.size()==1 && std::strcmp(input.format_name(),"openexr")==0 && threads==1 &&
        (compression=="zip" || compression=="zips") && !spec.tile_width && layout.layers.size()>1) {
        for(const auto& layer:layout.layers)
            combined_channels.insert(combined_channels.end(),layer.channels.begin(),layer.channels.end());
        const auto combined=channel_plan(spec,combined_channels,true);
        strips=combined.runs.size()==1 && std::size_t(spec.height)*source_bytes/combined.pixel_bytes>=64;
    }
    const int rows=strips?64:spec.height;
    const std::size_t batches=strips?1:layout.layers.size();
    OIIO::ImageSpec strip_spec;
    if(strips) {
        // channel_plan needs only geometry and native types, not metadata or
        // channel names. In particular, never copy an embedded manifest here.
        strip_spec.width=spec.width;strip_spec.nchannels=spec.nchannels;
        strip_spec.format=spec.format;strip_spec.channelformats=spec.channelformats;
    }
    for(int row=0;row<spec.height;row+=rows)for(std::size_t batch=0;batch<batches;++batch) {
        if(strips)strip_spec.height=std::min(rows,spec.height-row);
        const auto& layer=layout.layers[batch];
        const auto& source=layout.parts.at(layer.part);
        const auto& strip=strips?strip_spec:source;
        const auto& selected=strips?combined_channels:layer.channels;
        const auto plan=channel_plan(strip,selected,true);
        std::vector<unsigned char> raw(std::size_t(spec.width)*strip.height*plan.pixel_bytes);
        auto tick=Clock::now();
        const bool read=strips?input.read_scanlines(layer.part,0,source.y+row,source.y+row+strip.height,source.z,
            plan.runs.front().first,plan.runs.front().end,OIIO::TypeDesc::UNKNOWN,raw.data()):
            read_selected(input,source,layer.part,plan,true,raw.data());
        if(!read)throw FileFailure("decode_failed",input.geterror());
        result.decode_ms+=ms(tick);tick=Clock::now();
        struct Rank {const unsigned char* id;const unsigned char* coverage;std::size_t id_stride,coverage_stride;bool half_coverage;};
        std::vector<Rank> ranks;
        for(std::size_t pair=0;pair<selected.size();pair+=2) {
            auto ii=std::size_t(std::lower_bound(plan.channels.begin(),plan.channels.end(),selected[pair])-plan.channels.begin());
            auto ci=std::size_t(std::lower_bound(plan.channels.begin(),plan.channels.end(),selected[pair+1])-plan.channels.begin());
            ranks.push_back({raw.data()+plan.offsets[ii],raw.data()+plan.offsets[ci],plan.strides[ii],plan.strides[ci],source.channelformat(selected[pair+1])==OIIO::TypeDesc::HALF});
        }
        OIIO::parallel_for_chunked(0,strip.height,0,[&](int64_t begin,int64_t end) {
            for(std::size_t p=std::size_t(begin)*spec.width;p<std::size_t(end)*spec.width;++p)for(const auto& rank:ranks) {
                const auto destination=std::size_t(row)*spec.width+p;
                float weight=coverage(rank.coverage+p*rank.coverage_stride,rank.half_coverage);
                if(!std::isfinite(weight)){bad[destination]=1;continue;}
                if(weight==0)continue; // Unused ranks may contain arbitrary ID padding.
                auto bits=value<std::uint32_t>(rank.id+p*rank.id_stride);
                if((bits&0x7fffffffU)==0)continue; // Null/background ID, including signed zero.
                auto exponent=(bits>>23)&255U;if(exponent==0 || exponent==255){bad[destination]=1;continue;}
                float colour[4];id_palette(bits,colour);
                for(int c=0;c<3;++c)sums[destination*4+c]+=double(weight)*colour[c];
                sums[destination*4+3]+=double(weight);
            }
        },OIIO::paropt(threads,OIIO::paropt::SplitDir::Y,1));
        result.mapping_ms+=ms(tick);
    }
    auto tick=Clock::now();auto mapped=spec;mapped.nchannels=4;mapped.set_format(OIIO::TypeDesc::FLOAT);mapped.channelnames={"R","G","B","A"};mapped.alpha_channel=3;mapped.z_channel=-1;
    mapped.erase_attribute("ICCProfile");mapped.attribute("oiio:UnassociatedAlpha",0);result.image.reset(mapped);
    auto* target=static_cast<float*>(result.image.localpixels());if(!target)throw FileFailure("conversion_failed","cannot allocate Cryptomatte overview");
    for(std::size_t p=0;p<pixels;++p) {
        auto* out=target+p*4;
        if(bad[p]){++result.statistics.invalid_pixels;out[0]=out[2]=invalid==InvalidData::magenta?1.0f:0.0f;out[1]=0;out[3]=1;continue;}
        auto alpha=std::clamp(sums[p*4+3],0.0,1.0);if(alpha!=sums[p*4+3])++result.statistics.clipped_components;out[3]=float(alpha);
        for(int c=0;c<3;++c){auto channel=std::clamp(sums[p*4+c],0.0,alpha);if(channel!=sums[p*4+c])++result.statistics.clipped_components;out[c]=float(channel);}
    }
    result.mapping_ms+=ms(tick);
    if(invalid==InvalidData::error && result.statistics.invalid_pixels)throw InvalidDataValues(result.statistics);
    return result;
}
}
