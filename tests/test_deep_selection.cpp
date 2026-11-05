#include "deep_selection.h"
#include "deep_flatten.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace thumbgen::detail;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static OIIO::ImageSpec spec(std::vector<std::string> names,const char* part="") {
    OIIO::ImageSpec value(2,3,int(names.size()),OIIO::TypeDesc::FLOAT);value.deep=true;value.channelnames=std::move(names);
    value.attribute("oiio:subimagename",part);return value;
}
static Candidate candidate(const OIIO::ImageSpec& value,const std::string& group="RGB") {
    auto candidates=deep_colour_candidates(value);
    auto found=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){return c.group==group;});
    require(found!=candidates.end(),"expected Deep colour candidate missing");return *found;
}
static DeepSelection select(const OIIO::ImageSpec& value,const std::string& group="RGB") {return deep_selection(value,candidate(value,group));}
template<class F> static void rejected(F function) {
    try{function();}catch(const DeepFailure& failure){require(std::string(failure.code)=="deep_channel_mapping_invalid","unexpected mapping error");return;}
    throw std::runtime_error("invalid Deep mapping accepted");
}
int main() {
    try {
        auto root=spec({"R","G","B","A","Z","ZBack"});auto scalar=select(root);
        require(scalar.channels.colour==std::array<std::string,3>{"R","G","B"} && scalar.shared_alpha &&
            scalar.channels.alpha==std::array<std::string,3>{"A","A","A"} && scalar.channels.z=="Z" && scalar.channels.zback=="ZBack","root mapping differs");
        auto shuffled=root;std::reverse(shuffled.channelnames.begin(),shuffled.channelnames.end());
        require(select(shuffled).identity()==scalar.identity(),"channel reordering changed Deep identity");
        auto points=spec({"R","G","B","A","Z"});require(select(points).channels.zback.empty() && select(points).identity()!=scalar.identity(),"point/volume identity not distinct");
        auto triple=spec({"R","G","B","A","AR","AG","AB","Z"});auto coloured=select(triple);
        require(!coloured.shared_alpha && coloured.channels.alpha==std::array<std::string,3>{"AR","AG","AB"},"complete alpha triple did not take precedence");
        require(coloured.identity()!=select(points).identity(),"alpha mapping missing from identity");
        auto layered=spec({"beauty.R","beauty.G","beauty.B","beauty.A","Z","Zback","A","AR","AG","AB","R","G","B","alpha.R","alpha.G","alpha.B"});
        auto beauty=candidate(layered,"beauty");auto local=deep_selection(layered,beauty);
        require(is_primary(layered,beauty) && local.shared_alpha && local.channels.alpha[0]=="beauty.A" && local.channels.z=="Z","local scalar/root depth mapping differs");
        auto choices=deep_colour_candidates(layered);require(choices.size()==2,"alpha RGB planes became image candidates");
        // Sample values do not enter this module; identities depend solely on metadata.
        auto changed=layered;changed.channelnames.erase(std::find(changed.channelnames.begin(),changed.channelnames.end(),"beauty.A"));--changed.nchannels;
        auto fallback=select(changed,"beauty");require(fallback.channels.alpha[0]=="AR" && fallback.identity()!=local.identity(),"local alpha disappearance silently preserved identity");
        auto local_depth=spec({"beauty.R","beauty.G","beauty.B","beauty.A","beauty.Z","Z","Zback"});
        auto mapped=select(local_depth,"beauty");require(mapped.channels.z=="Z" && mapped.channels.zback=="Zback","layer auxiliary depth replaced base-layer geometry");
        auto nested=spec({"view.beauty.r","view.beauty.g","view.beauty.b","view.beauty.ar","view.beauty.ag","view.beauty.ab","view.beauty.z","view.beauty.zback","Z","ZBack"});
        require(select(nested,"view.beauty").channels.zback=="ZBack","nested colour did not use base depth");
        auto parents=spec({"view.beauty.R","view.beauty.G","view.beauty.B","view.beauty.AR","view.AG","view.A","A","AB","Z"});
        auto inherited=select(parents,"view.beauty");
        require(inherited.channels.alpha==std::array<std::string,3>{"view.beauty.AR","view.AG","view.A"},"per-component alpha did not follow enclosing layers");
        auto mixed=spec({"R","G","B","AR","AG","A","Z"});
        require(select(mixed).channels.alpha==std::array<std::string,3>{"AR","AG","A"},"valid partial alpha triple was refused");
        auto grey=spec({"Y","A","Z","Zback"});auto grey_mapping=select(grey,"GRAY");
        require(grey_mapping.channels.colour==std::array<std::string,3>{"Y","Y","Y"},"Deep luminance support planes broke classification");
        auto grey_alpha=spec({"Y","A","AR","AG","AB","Z"});
        require(select(grey_alpha,"GRAY").channels.alpha==std::array<std::string,3>{"A","A","A"},"Y incorrectly used AR/AG/AB");
        auto nested_grey=spec({"view.beauty.Y","view.beauty.AR","view.A","A","Z"});
        require(select(nested_grey,"view.beauty").channels.alpha==std::array<std::string,3>{"view.A","view.A","view.A"},"nested luminance did not inherit A");
        for(const char* part:{"normal","depth","alpha","opacity","transmission"})
            require(deep_colour_candidates(spec({"R","G","B","A","Z"},part)).empty(),"data/alpha part treated as main image");
        for(const char* group:{"normal","cryptoObject00","opacity","alpha","transmittance"}) {
            std::string p=group;require(deep_colour_candidates(spec({p+".R",p+".G",p+".B","A","Z"})).empty(),"data/alpha group treated as image");
        }
        for(auto invalid:std::vector<OIIO::ImageSpec>{
            spec({"R","G","B","Z"}),spec({"R","G","B","A"}),
            spec({"R","G","B","AR","Z"}),spec({"R","G","B","AR","AG","Z"}),
            spec({"R","G","B","A","a","Z"}),spec({"R","G","B","A","Z","z"}),
            spec({"R","G","B","AR","ar","AG","AB","Z"}),
            spec({"R","G","B","A","Z","ZBack","zback"}),
            spec({"beauty.R","beauty.G","beauty.B","beauty.AR","Z"}),
            spec({"beauty.R","beauty.G","beauty.B","beauty.A","beauty.Z"})})
            rejected([&]{select(invalid,invalid.channelnames[0].find('.')==std::string::npos?"RGB":"beauty");});
        rejected([&]{select(spec({"Y","AR","AG","AB","Z"}),"GRAY");});
        auto auxiliary=local_depth;auxiliary.channelformats.assign(auxiliary.nchannels,OIIO::TypeDesc::FLOAT);auxiliary.channelformats[4]=OIIO::TypeDesc::UINT;
        require(select(auxiliary,"beauty").identity()==mapped.identity(),"unused layer-local UINT depth affected mapping");
        for(int index:{0,3,4,5}) {
            auto wrong=root;wrong.channelformats.assign(root.nchannels,OIIO::TypeDesc::FLOAT);wrong.channelformats[index]=OIIO::TypeDesc::UINT;
            rejected([&]{select(wrong);});
            wrong.channelformats[index]=OIIO::TypeDesc::HALF;select(wrong);
        }
        auto extra=root;extra.channelnames.push_back("objectID");++extra.nchannels;
        extra.channelformats.assign(extra.nchannels,OIIO::TypeDesc::FLOAT);extra.channelformats.back()=OIIO::TypeDesc::UINT;
        require(select(extra).identity()==scalar.identity(),"unused UINT AOV affected mapping");
        auto bad=candidate(root);bad.channels[2]=999;rejected([&]{deep_selection(root,bad);});
        thumbgen::Json pin{{"key",{{"part",0},{"part_name",""},{"group","RGB"},{"kind","colour"},{"channels",{"R","G","B"}},{"deep",scalar.identity()}}},
            {"request",{{"part",-1},{"part_name",""},{"channels",""},{"strategy","primary"},{"deep_mode","flatten"}}},{"named",false}};
        thumbgen::Engine engine;engine.restore_sequence_lock("mapping-v2",pin);
        require(engine.sequence_lock("mapping-v2")==pin,"v2 lock roundtrip failed");
        for(const char* invalid:{"old-policy","local-depth","unrelated-alpha","wrong-component"}) {
            auto malformed=pin;auto& mapping=malformed["key"]["deep"];
            if(std::string(invalid)=="old-policy")mapping["policy"]="deep-mapping-v1";
            if(std::string(invalid)=="local-depth")mapping["z"]="beauty.Z";
            if(std::string(invalid)=="unrelated-alpha"){mapping["alpha"]={"other.A","other.A","other.A"};}
            if(std::string(invalid)=="wrong-component"){mapping["alpha"]={"AG","AG","AG"};}
            try{engine.restore_sequence_lock(invalid,malformed);throw std::runtime_error("malformed v2 lock accepted");}catch(const std::invalid_argument&){}
        }
        for(int mode=0;mode<4;++mode) {
            auto malformed=root;
            if(mode==0)malformed.channelnames.pop_back();
            if(mode==1)malformed.channelformats.assign(1,OIIO::TypeDesc::FLOAT);
            if(mode==2)malformed.deep=false;
            if(mode==3)malformed.nchannels=4097;
            rejected([&]{deep_colour_candidates(malformed);});
            rejected([&]{deep_selection(malformed,candidate(root));});
        }
        std::cout<<"Deep metadata mapping, exact sequence identity, alpha/depth precedence and ambiguous-layout guards passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
