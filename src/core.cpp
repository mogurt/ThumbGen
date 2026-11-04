#include <thumbgen/core.h>
#include "selection.h"
#include "build_info.h"
#include <OpenImageIO/color.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace thumbgen {
namespace fs = std::filesystem;
namespace iba = OIIO::ImageBufAlgo;
using Clock = std::chrono::steady_clock;
struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string c, std::string message) : std::runtime_error(message), code(std::move(c)) {}
};
static void check(bool ok, const OIIO::ImageBuf& image, const char* code) {
    if (!ok) throw Failure(code,image.geterror());
}
static double ms(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
static Json spec_json(const OIIO::ImageSpec& s, int part) {
    return {{"part",part},{"name",s.get_string_attribute("oiio:subimagename")},
        {"width",s.width},{"height",s.height},{"channels",s.channelnames},
        {"sample_type",s.format.c_str()},{"deep",s.deep},
        {"compression",s.get_string_attribute("compression")},
        {"data_window",{s.x,s.y,s.width,s.height}},
        {"display_window",{s.full_x,s.full_y,s.full_width,s.full_height}},
        {"colour_space",s.get_string_attribute("oiio:ColorSpace")}};
}
static Json candidates_json(const OIIO::ImageSpec& spec,int part) {
    Json result=Json::array();
    for(const auto& candidate:detail::classify(spec))result.push_back(detail::candidate_json(spec,candidate,part));
    return result;
}

struct Engine::Impl {
    struct Pin {Json key,request;bool named;};
    std::map<std::string,Pin> sequences;
    std::map<std::string,std::unique_ptr<OIIO::ColorConfig>> configs;
    OIIO::ColorConfig* config(const std::string& name) {
        auto found=configs.find(name);
        if(found!=configs.end())return found->second.get();
        auto value=std::make_unique<OIIO::ColorConfig>(name);
        // Do not cache failed configs: geterror() clears the error, and a later
        // job must not accidentally reuse OIIO's fallback as a valid config.
        if(value->has_error())throw Failure("colour_config_failed",value->geterror());
        auto* result=value.get();configs.emplace(name,std::move(value));return result;
    }
};
Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

Json Engine::capabilities() {
    Json output_formats=Json::array();
    for(const char* format:{"jpeg","png","webp"})
        if(OIIO::ImageOutput::create(std::string("capability.")+format))output_formats.push_back(format);
    return {{"schema_version",1},{"type","capabilities"},{"version","0.1.0-dev"},
        {"oiio_version",OIIO_VERSION_STRING},{"input_plugins",OIIO::get_string_attribute("input_format_list")},
        {"output_plugins",OIIO::get_string_attribute("output_format_list")},
        {"build",{{"revision",THUMBGEN_SOURCE_REVISION},{"dirty",THUMBGEN_SOURCE_DIRTY},
            {"compiler",THUMBGEN_COMPILER},{"platform",THUMBGEN_PLATFORM},{"architecture",THUMBGEN_ARCH}}},
        {"cpu",{{"runtime_simd",OIIO::get_string_attribute("hw:simd")},{"oiio_simd",OIIO::get_string_attribute("build:simd")}}},
        {"selection_policy","v2"},
        {"libraries",OIIO::get_string_attribute("library_list")},
        {"output_formats",output_formats},{"presets",{"fast","balanced","quality"}},
        {"defaults",{{"format","jpeg"},{"quality",90},{"size",{1024,1024}},
                     {"background","black"},{"preset","balanced"}}},
        {"limitations",{"no Deep flattening or Cryptomatte visualization yet",
            "batch is serial across files; native library threads are bounded",
            "max_decode_mib limits decoded source buffer, not process RSS"}}};
}

Json Engine::inspect(const std::string& input) {
    Json result{{"schema_version",1},{"type","inspection"},{"input",input},{"ok",false}};
    auto in=OIIO::ImageInput::open(input);
    if(!in){result["error"]={{"code","open_failed"},{"message",OIIO::geterror()}};return result;}
    result["parts"]=Json::array();
    OIIO::ImageSpec spec;
    for(int part=0;in->seek_subimage(part,0,spec);++part) {
        auto value=spec_json(spec,part);value["candidates"]=candidates_json(spec,part);
        result["parts"].push_back(value);
    }
    if(result["parts"].empty()) {
        result["error"]={{"code","metadata_failed"},{"message",in->geterror()}};
        return result;
    }
    result["ok"]=true;
    return result;
}

Json Engine::convert(const Job& job) {
    auto started=Clock::now();
    Json r{{"schema_version",1},{"type","result"},{"id",job.id},{"input",job.input},
           {"ok",false},{"status","failed"},{"outputs",Json::array()},{"warnings",Json::array()}};
    if(!job.sequence_id.empty())r["sequence_id"]=job.sequence_id;
    std::string stage="open";
    try {
        OIIO::attribute("threads",job.options.threads);
        OIIO::attribute("exr_threads",job.options.threads);
        auto tick=Clock::now();
        OIIO::ImageSpec read_config;
        // Ask unassociated formats to return straight RGB. In particular WebP
        // and PNG otherwise associate their nonlinear RGB in different spaces.
        read_config.attribute("oiio:UnassociatedAlpha",1);
        auto in=OIIO::ImageInput::open(job.input,&read_config);
        if(!in)throw Failure("open_failed",OIIO::geterror());
        stage="selection";
        OIIO::ImageSpec spec;
        std::vector<int> channels;
        int part=-1;
        const std::string selection_basis=job.options.part>=0 || !job.options.part_name.empty()?
            (job.options.channels.empty()?"explicit_part":"explicit_part_channels"):
            (job.options.channels.empty()?"automatic":"explicit_channels");
        r["selection"]={{"policy","v2"},{"strategy",job.options.selection_policy},{"basis",selection_basis},{"lock_status","none"},
            {"candidates",Json::array()}};
        struct Choice {int part;OIIO::ImageSpec spec;detail::Candidate candidate;};
        std::vector<Choice> choices,all_choices;
        int part_count=0;
        bool part_exists=false,has_deep=false,has_data=false,matched_group=false;
        for(int p=0;;++p) {
            OIIO::ImageSpec candidate;
            if(!in->seek_subimage(p,0,candidate))break;
            ++part_count;
            auto groups=detail::classify(candidate);
            for(const auto& group:groups)r["selection"]["candidates"].push_back(detail::candidate_json(candidate,group,p));
            if(job.options.part>=0 && p!=job.options.part)continue;
            if(!job.options.part_name.empty() && candidate.get_string_attribute("oiio:subimagename")!=job.options.part_name)continue;
            part_exists=true;has_deep=has_deep || candidate.deep;
            std::vector<detail::Candidate> eligible;
            for(const auto& group:groups) {
                if(!job.options.channels.empty() && group.group!=job.options.channels)continue;
                matched_group=true;has_data=has_data || group.kind=="data";
                if(group.kind=="colour" || group.kind=="grey") {
                    eligible.push_back(group);all_choices.push_back({p,candidate,group});
                }
            }
            // Prefer root RGB within a part. Other parts still participate in
            // ambiguity checks; do not silently skip a part with several groups.
            if(job.options.channels.empty()) {
                bool primary=job.options.selection_policy=="primary" && std::any_of(eligible.begin(),eligible.end(),[&](const auto& c){return detail::is_primary(candidate,c);});
                if(primary)eligible.erase(std::remove_if(eligible.begin(),eligible.end(),[&](const auto& c){return !detail::is_primary(candidate,c);}),eligible.end());
                else {
                    auto root=std::find_if(eligible.begin(),eligible.end(),[](const auto& c){return c.group=="RGB";});
                    if(root!=eligible.end()){auto chosen=*root;eligible={std::move(chosen)};}
                }
            }
            for(const auto& group:eligible)choices.push_back({p,candidate,group});
        }
        if(job.options.channels.empty() && job.options.selection_policy=="primary" &&
           std::any_of(choices.begin(),choices.end(),[](const auto& c){return detail::is_primary(c.spec,c.candidate);}))
            choices.erase(std::remove_if(choices.begin(),choices.end(),[](const auto& c){return !detail::is_primary(c.spec,c.candidate);}),choices.end());
        Json request={{"part",job.options.part},{"part_name",job.options.part_name},
            {"channels",job.options.channels},{"strategy",job.options.selection_policy}};
        auto pin=impl_->sequences.find(job.sequence_id);
        auto key=[](const Choice& choice) {
            auto value=detail::candidate_json(choice.spec,choice.candidate,choice.part);
            // Named parts may move in the file without changing their identity.
            if(!value["part_name"].get<std::string>().empty())value.erase("part");
            value.erase("reason");value.erase("primary");return value;
        };
        if(!job.sequence_id.empty() && pin!=impl_->sequences.end()) {
            r["selection"]["lock_status"]="reused";r["selection"]["locked"]=pin->second.key;
            if(request!=pin->second.request)throw Failure("sequence_selection_conflict","selection options differ within sequence_id");
            choices.clear();
            if(pin->second.named || part_count==1)
                for(const auto& choice:all_choices)if(key(choice)==pin->second.key)choices.push_back(choice);
            if(choices.size()!=1)throw Failure("sequence_selection_changed","locked part/group/channel mapping is missing or ambiguous; no fallback");
        }
        if(!part_exists)throw Failure("part_not_found","requested part does not exist");
        if(choices.empty()) {
            if(has_deep)throw Failure("deep_requires_flattening","Deep data requires flattening");
            if(has_data)throw Failure("visualization_required","data AOV requires dedicated visualization; colour selection cannot override it");
            if(!job.options.channels.empty() && !matched_group)throw Failure("channels_not_found","requested channel group does not exist");
            throw Failure("ambiguous_channels","no recognized colour or luminance layout; inspect candidates");
        }
        if(choices.size()!=1) {
            bool same=std::all_of(choices.begin(),choices.end(),[&](const auto& c){return c.part==choices[0].part;});
            throw Failure(same?"ambiguous_channels":"ambiguous_parts","multiple colour candidates; specify part and/or channels");
        }
        auto chosen=std::move(choices[0]);const Json chosen_key=key(chosen);
        part=chosen.part;spec=std::move(chosen.spec);channels=chosen.candidate.channels;
        r["selection"]["chosen"]=detail::candidate_json(spec,chosen.candidate,part);
        if(spec.width<1 || spec.height<1 || spec.depth!=1 || spec.full_width<1 || spec.full_height<1)
            throw Failure("invalid_dimensions","unsupported image dimensions");
        if(!job.sequence_id.empty() && pin==impl_->sequences.end()) {
            bool named=!spec.get_string_attribute("oiio:subimagename").empty();
            if(!named && part_count!=1)throw Failure("sequence_part_identity_required","sequence selection requires unique named parts for multipart files");
            impl_->sequences.emplace(job.sequence_id,Impl::Pin{chosen_key,request,named});
            r["selection"]["lock_status"]="created";r["selection"]["locked"]=chosen_key;
        }
        r["source"]=spec_json(spec,part);
        r["selected_channels"]=Json::array();
        for(int c:channels)r["selected_channels"].push_back(spec.channelnames[c]);
        r["timings_ms"]["open_select"]=ms(tick);
        stage="colour";
        std::string config_name=job.options.ocio_config,config_basis="explicit";
        if(config_name.empty()) {
            const char* environment=std::getenv("OCIO");
            config_name=environment?environment:"";config_basis="environment";
            if(config_name.empty()){config_name="ocio://default";config_basis="builtin";}
        }
        r["colour"]={{"ocio_config",config_name},{"config_basis",config_basis},
            {"input_requested",job.options.input_space},{"display",job.options.display},{"view",job.options.view}};
        auto* config=impl_->config(config_name);
        auto space=[&](const std::string& name) {
            if(config->getColorSpaceIndex(name)<0)
                throw Failure("colour_space_not_found","colour space or role not found: "+name);
            return std::string(config->resolve(name));
        };
        const auto roles=config->getRoles();
        if(std::find(roles.begin(),roles.end(),"scene_linear")==roles.end())
            throw Failure("colour_working_space_missing","OCIO config must define the scene_linear role");
        const std::string working_space=space("scene_linear");
        if(!config->isColorSpaceLinear(working_space))
            throw Failure("colour_working_space_invalid","scene_linear role must use a linear colour space");
        std::string from=job.options.input_space,basis="explicit";
        if(from=="auto") {
            from=spec.get_string_attribute("oiio:ColorSpace");basis="metadata";
            if(from.empty()) {
                auto ext=fs::u8path(job.input).extension().u8string();
                std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
                from=ext==".exr"||ext==".hdr"?"linear":"sRGB";basis="format_assumption";
                r["warnings"].push_back("input colour space assumed from format; override with input_space");
            }
        }
        const std::string input_space=space(from);
        std::string output_space;
        if(job.options.display.empty())output_space=space("sRGB");
        else {
            const auto displays=config->getDisplayNames();
            if(std::find(displays.begin(),displays.end(),job.options.display)==displays.end())
                throw Failure("colour_display_not_found","display not found: "+job.options.display);
            const auto views=config->getViewNames(job.options.display);
            if(std::find(views.begin(),views.end(),job.options.view)==views.end())
                throw Failure("colour_view_not_found","view not found: "+job.options.view);
        }
        r["colour"]={{"input_space",input_space},{"input_requested",from},{"basis",basis},
            {"working_space",working_space},{"output_space",output_space},{"ocio_config",config->configname()},
            {"config_basis",config_basis},{"display",job.options.display},{"view",job.options.view}};
        auto bounds=std::minmax_element(channels.begin(),channels.end());
        int first=*bounds.first,end=*bounds.second+1;
        const size_t limit=job.options.max_decode_mib*1024ULL*1024ULL/sizeof(float);
        size_t count=size_t(spec.width);
        for(size_t factor:{size_t(spec.height),size_t(end-first)}) {
            if(factor==0 || count>limit/factor)throw Failure("decode_limit_exceeded","decoded source buffer exceeds limit");
            count*=factor;
        }
        stage="decode";tick=Clock::now();
        std::vector<float> pixels(count);
        bool read_ok=in->read_image(part,0,first,end,OIIO::TypeDesc::FLOAT,pixels.data());
        if(!read_ok)throw Failure("decode_failed",in->geterror());
        in->close();
        r["timings_ms"]["decode"]=ms(tick);
        r["decoded_mib"]=double(count*sizeof(float))/1048576;
        OIIO::ImageSpec subset=spec;
        subset.nchannels=end-first;subset.format=OIIO::TypeDesc::FLOAT;subset.channelformats.clear();
        subset.channelnames.assign(spec.channelnames.begin()+first,spec.channelnames.begin()+end);
        subset.alpha_channel=spec.alpha_channel>=first && spec.alpha_channel<end ? spec.alpha_channel-first:-1;
        OIIO::ImageBuf buffer(subset,pixels.data()),working;
        std::vector<int> order;for(int c:channels)order.push_back(c-first);
        std::vector<std::string> names{"R","G","B"};if(order.size()==4)names.push_back("A");
        check(iba::channels(working,buffer,int(order.size()),order,{},names),working,"channels_failed");
        if(spec.get_int_attribute("oiio:UnassociatedAlpha",0) && order.size()==4)
            check(iba::premult(working,working),working,"alpha_failed");
        working.specmod().attribute("oiio:UnassociatedAlpha",0);
        // resize() clamps outside the data window. Pad a cropped display window
        // with zero pixels first, rather than stretching edge pixels into it.
        const auto& window=working.spec();
        if(window.x!=window.full_x || window.y!=window.full_y ||
           window.width!=window.full_width || window.height!=window.full_height) {
            size_t canvas=size_t(window.full_width);
            for(size_t factor:{size_t(window.full_height),size_t(working.nchannels())}) {
                if(factor==0 || canvas>limit/factor)throw Failure("canvas_limit_exceeded","display canvas exceeds pixel buffer limit");
                canvas*=factor;
            }
            auto xmax=static_cast<long long>(window.full_x)+window.full_width;
            auto ymax=static_cast<long long>(window.full_y)+window.full_height;
            if(xmax>std::numeric_limits<int>::max()||ymax>std::numeric_limits<int>::max())
                throw Failure("invalid_dimensions","display window endpoint overflow");
            OIIO::ImageBuf padded;
            check(iba::crop(padded,working,OIIO::ROI(window.full_x,int(xmax),window.full_y,int(ymax),0,1,0,working.nchannels())),padded,"window_failed");
            working=std::move(padded);
        }
        // Orient a complete, zero-origin display canvas. Some OIIO rotations
        // position cropped data incorrectly when the full window has an origin.
        working.set_origin(0,0,0);
        working.set_roi_full(working.roi());
        if(spec.get_int_attribute("Orientation",1)!=1) {
            OIIO::ImageBuf oriented;
            check(iba::reorient(oriented,working),oriented,"orientation_failed");working=std::move(oriented);
        }
        stage="colour";tick=Clock::now();
        if(spec.find_attribute("ICCProfile"))r["warnings"].push_back("embedded ICC profile is not independently transformed; verify input_space against its profile");
        if(input_space!=working_space)
            check(iba::colorconvert(working,working,input_space,working_space,true,"","",config),working,"colour_failed");
        r["timings_ms"]["working_colour"]=ms(tick);
        const std::string filter=job.options.preset=="fast"?"box":job.options.preset=="quality"?"lanczos3":"triangle";
        r["preset"]=job.options.preset;r["filter"]=filter;
        for(const auto& out:job.outputs) {
            Json result{{"path",out.path},{"status","failed"}};
            fs::path temp;
            try {
                fs::path dest=fs::u8path(out.path);
                if(fs::exists(dest))throw Failure("output_exists","output exists; no implicit overwrite");
                fs::create_directories(dest.parent_path());
                const auto& s=working.spec();
                double scale=std::min({double(out.width)/s.full_width,double(out.height)/s.full_height,1.0});
                int w=std::max(1,int(std::lround(s.full_width*scale))),h=std::max(1,int(std::lround(s.full_height*scale)));
                OIIO::ImageBuf preview;
                tick=Clock::now();
                check(iba::resize(preview,working,{{"filtername",filter}},OIIO::ROI(0,w,0,h,0,1,0,int(order.size())),job.options.threads),preview,"resize_failed");
                result["resize_ms"]=ms(tick);tick=Clock::now();
                if(job.options.background!="transparent" && preview.nchannels()==4) {
                    OIIO::ImageSpec bs(w,h,4,OIIO::TypeDesc::FLOAT);bs.alpha_channel=3;
                    OIIO::ImageBuf background(bs),composite;
                    float value=job.options.background=="white"?1.0f:0.0f;
                    std::vector<float> colour{value,value,value,1.0f};
                    check(iba::fill(background,colour),background,"alpha_failed");
                    check(iba::over(composite,preview,background),composite,"alpha_failed");preview=std::move(composite);
                }
                if(job.options.display.empty())
                    check(iba::colorconvert(preview,preview,working_space,output_space,true,"","",config),preview,"colour_failed");
                else
                    check(iba::ociodisplay(preview,preview,job.options.display,job.options.view,working_space,"",true,false,"","",config),preview,"colour_failed");
                if(job.options.background=="transparent" && preview.nchannels()==4) {
                    // Supply straight display RGB explicitly. PNG and WebP plugins
                    // otherwise deassociate in different spaces (WebP uses gamma 2.2).
                    check(iba::unpremult(preview,preview),preview,"alpha_failed");
                    preview.specmod().attribute("oiio:UnassociatedAlpha",1);
                }
                if(job.options.background!="transparent" || out.format=="jpeg") {
                    OIIO::ImageBuf rgb;std::vector<int> rgb_order{0,1,2};
                    check(iba::channels(rgb,preview,3,rgb_order),rgb,"channels_failed");preview=std::move(rgb);
                }
                preview.specmod().attribute("CompressionQuality",out.quality);
                preview.specmod().attribute("oiio:ColorSpace",job.options.display.empty()?"sRGB":"display-referred");
                static std::atomic<unsigned long long> serial{0};
#ifdef _WIN32
                auto pid=GetCurrentProcessId();
#else
                auto pid=getpid();
#endif
                temp=dest.parent_path()/fs::u8path("."+dest.filename().u8string()+".tmp-"+std::to_string(pid)+"-"+std::to_string(++serial)+"."+out.format);
                check(preview.write(temp.u8string(),OIIO::TypeDesc::UINT8),preview,"encode_failed");
#ifdef _WIN32
                if(!MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_WRITE_THROUGH))
                    throw Failure("commit_failed","cannot commit output without overwriting existing file");
#else
                if(::link(temp.c_str(),dest.c_str())!=0)throw Failure("commit_failed","cannot commit output without overwriting existing file");
                fs::remove(temp);
#endif
                result["status"]="success";result["width"]=w;result["height"]=h;
                result["finish_ms"]=ms(tick);result["format"]=out.format;
            }catch(const Failure& e){result["error"]={{"code",e.code},{"message",e.what()}};}
             catch(const std::exception& e){result["error"]={{"code","output_failed"},{"message",e.what()}};}
            if(!temp.empty()){std::error_code ec;fs::remove(temp,ec);}
            r["outputs"].push_back(result);
        }
        size_t successes=0;for(const auto& output:r["outputs"])if(output["status"]=="success")++successes;
        r["ok"]=successes==job.outputs.size();
        r["status"]=r["ok"].get<bool>()?"success":successes?"partial":"failed";
    }catch(const Failure& e){r["error"]={{"code",e.code},{"message",e.what()},{"stage",stage}};}
     catch(const std::exception& e){r["error"]={{"code","conversion_failed"},{"message",e.what()},{"stage",stage}};}
    r["timings_ms"]["total"]=ms(started);
    return r;
}
}
