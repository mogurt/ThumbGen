#include <thumbgen/core.h>
#include "selection.h"
#include "exr_profile.h"
#include "color_input.h"
#include "cms_input.h"
#include "colour_plan.h"
#include "png_scanline.h"
#ifdef THUMBGEN_C2_PNG_STREAM
#include "c2_resize.h"
#include "c2_png_rows.h"
#include "c2_memory.h"
#endif
#include "cmyk_jpeg.h"
#include "build_info.h"
#include "files.h"
#include "quantize.h"
#include "resize_srgb.h"
#include "data_image.h"
#include "selected_input.h"
#include "cryptomatte.h"
#include "sampled_id.h"
#include "deep_selection.h"
#include "deep_flatten.h"
#include <OpenImageIO/color.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/parallel.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <set>
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
using Failure=detail::FileFailure;
// OIIO's IOFile does not propagate fflush/fclose errors. Keep ownership of
// the stream, and latch short writes even when a codec ignores their result.
static void write_preview(const OIIO::ImageBuf& source,const fs::path& path,int threads,bool canonical_rgba TG_EXR_PROFILE_PARAM) {
    const auto& original=source.spec();
    if(original.format!=OIIO::TypeDesc::FLOAT || original.depth!=1 || original.deep ||
       (original.nchannels!=3 && original.nchannels!=4) || !source.localpixels() || !source.contiguous())
        throw Failure("encode_failed","expected a contiguous FLOAT RGB/RGBA preview");
    TG_EXR_TICK(quantize_tick);
    auto spec=original;spec.set_format(OIIO::TypeDesc::UINT8);
    spec.attribute("oiio:BitsPerSample",8);
    OIIO::ImageBuf image(spec);
    if(!image.localpixels() || !image.contiguous())throw Failure("encode_failed","cannot allocate UINT8 preview");
    const auto* input=static_cast<const float*>(source.localpixels());
    auto* output=static_cast<std::uint8_t*>(image.localpixels());
    const auto stride=canonical_rgba?4:1;
    const auto count=original.image_bytes()/sizeof(float)/stride;
    OIIO::parallel_for_chunked(0,static_cast<int64_t>(count),0,[&](int64_t begin,int64_t end) {
        if(canonical_rgba)
            detail::quantize_rgba8(input+begin*4,output+begin*4,static_cast<size_t>(end-begin));
        else detail::quantize_u8(input+begin,output+begin,static_cast<size_t>(end-begin));
    },OIIO::paropt(threads,OIIO::paropt::SplitDir::Y,65536));
    TG_EXR_TOCK(quantize_tick,"exr_quantize_allocate_ms");
    TG_EXR_TICK(png_tick);
    struct CheckedFile final : OIIO::Filesystem::IOFile {
        int failure=0;
        explicit CheckedFile(FILE* file):IOFile(file,Write) {}
        // JPEG's IOFile fast path bypasses IOProxy and uses libjpeg's default
        // fatal stdio error handler (exit on ENOSPC). A distinct proxy type
        // selects OIIO's supported memory destination, followed by our checked
        // write. PNG/WebP continue to write through the proxy directly.
        const char* proxytype() const override {return "thumbgen_checked";}
        void failed() { if(!failure)failure=errno?errno:EIO; }
        size_t write(const void* data,size_t size) override {
            auto n=IOFile::write(data,size);if(n!=size)failed();return n;
        }
        size_t pwrite(const void* data,size_t size,int64_t offset) override {
            auto n=IOFile::pwrite(data,size,offset);if(n!=size)failed();return n;
        }
        bool seek(int64_t offset) override {
            auto ok=IOFile::seek(offset);if(!ok)failed();return ok;
        }
    };
    std::unique_ptr<FILE,decltype(&std::fclose)> stream(
        OIIO::Filesystem::fopen(path.u8string(),"w+b"),&std::fclose);
    if(!stream)throw Failure("encode_failed",detail::error_message({errno,std::generic_category()}));
    CheckedFile proxy(stream.get());
    image.set_write_ioproxy(&proxy);
    bool written;
    try { written=image.write(path.u8string(),OIIO::TypeDesc::UINT8); }
    catch(...) {image.set_write_ioproxy(nullptr);throw;}
    image.set_write_ioproxy(nullptr);
    TG_EXR_TOCK(png_tick,"exr_png_write_ms");
    TG_EXR_TICK(flush_tick);
    if(std::fflush(stream.get())!=0 || std::ferror(stream.get()))proxy.failed();
    if(std::fclose(stream.release())!=0)proxy.failed();
    TG_EXR_TOCK(flush_tick,"exr_output_close_ms");
    if(proxy.failure)throw Failure("encode_failed",detail::error_message({proxy.failure,std::generic_category()}));
    if(!written)throw Failure("encode_failed",image.geterror());
}
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
    Engine::Observer observer;
    Engine::Control control;
    bool converting=false;
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
Engine::Engine(Observer observer) : impl_(std::make_unique<Impl>()) {impl_->observer=std::move(observer);}
Engine::~Engine() = default;
void Engine::set_control(Control control) {
    if(impl_->converting)throw std::logic_error("cannot change control hooks during conversion");
    if(bool(control.reserve)!=bool(control.commit))throw std::invalid_argument("reserve and commit hooks must be paired");
    impl_->control=std::move(control);
}
Json Engine::sequence_lock(const std::string& id) const {
    auto pin=impl_->sequences.find(id);if(pin==impl_->sequences.end())return nullptr;
    return {{"key",pin->second.key},{"request",pin->second.request},{"named",pin->second.named}};
}
void Engine::forget_sequence_lock(const std::string& id) {
    if(impl_->converting)throw std::logic_error("cannot forget a sequence lock during conversion");
    impl_->sequences.erase(id);
}
void Engine::restore_sequence_lock(const std::string& id,const Json& lock) {
    if(impl_->converting)throw std::logic_error("cannot restore a sequence lock during conversion");
    auto valid=[](bool ok){if(!ok)throw std::invalid_argument("invalid internal sequence lock");};
    valid(!id.empty() && lock.is_object() && lock.size()==3 && lock.contains("key") && lock.contains("request") && lock.contains("named"));
    valid(lock["named"].is_boolean());const bool named=lock["named"].get<bool>();
    const auto& key=lock["key"];const auto& request=lock["request"];
    const bool deep=key.is_object() && key.contains("deep");
    valid(key.is_object() && key.size()==(named?4u:5u)+(deep?1u:0u));
    for(const char* field:{"part_name","group","kind"})valid(key.contains(field) && key[field].is_string());
    const bool data=request.is_object() && request.contains("data");
    valid(!key["group"].get<std::string>().empty() && (data?key["kind"]=="data":(key["kind"]=="colour" || key["kind"]=="grey")));
    valid(named?!key["part_name"].get<std::string>().empty():key["part_name"]=="");
    if(named)valid(!key.contains("part"));else valid(key.contains("part") && key["part"].is_number_integer() && key["part"]==0);
    valid(key.contains("channels") && key["channels"].is_array() && (data?key["channels"].size()>=1:(key["channels"].size()==3 || key["channels"].size()==4)));
    for(const auto& channel:key["channels"])valid(channel.is_string() && !channel.get<std::string>().empty());
    const bool deep_request=request.is_object() && request.contains("deep_mode");
    valid(request.is_object() && request.size()==4u+(data?1u:0u)+(deep_request?1u:0u));
    if(deep_request)valid(!data && request["deep_mode"]=="flatten");
    if(deep) {
        valid(deep_request && !data && key["channels"].size()==3);
        const auto& mapping=key["deep"];
        valid(mapping.is_object() && mapping.size()==6 && mapping.contains("policy") && mapping["policy"]=="deep-mapping-v2");
        valid(mapping.contains("colour") && mapping["colour"]==key["channels"] && mapping.contains("alpha") && mapping["alpha"].is_array() && mapping["alpha"].size()==3);
        for(const auto& name:mapping["alpha"])valid(name.is_string() && !name.get<std::string>().empty());
        valid(mapping.contains("z") && mapping["z"].is_string() && !mapping["z"].get<std::string>().empty() && mapping.contains("zback") && mapping["zback"].is_string());
        auto component=[](std::string name) {
            auto dot=name.rfind('.');if(dot!=std::string::npos)name=name.substr(dot+1);
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});return name;
        };
        auto layer=[](const std::string& name){auto dot=name.rfind('.');return dot==std::string::npos?std::string{}:name.substr(0,dot);};
        const auto z=mapping["z"].get<std::string>(),back=mapping["zback"].get<std::string>();
        valid(layer(z).empty() && component(z)=="z" && (back.empty() || (layer(back).empty() && component(back)=="zback")));
        for(int c=0;c<3;++c) {
            const auto colour_name=key["channels"][c].get<std::string>(),alpha_name=mapping["alpha"][c].get<std::string>();
            const auto colour_layer=layer(colour_name),alpha_layer=layer(alpha_name);
            const auto base=component(colour_name),alpha=component(alpha_name);
            valid(alpha_layer.empty() || alpha_layer==colour_layer || colour_layer.rfind(alpha_layer+".",0)==0);
            valid(alpha=="a" || (base=="r" && alpha=="ar") || (base=="g" && alpha=="ag") || (base=="b" && alpha=="ab"));
        }
        valid(mapping.contains("shared_alpha") && mapping["shared_alpha"].is_boolean() &&
            mapping["shared_alpha"]==(mapping["alpha"][0]==mapping["alpha"][1] && mapping["alpha"][0]==mapping["alpha"][2]));
    }
    if(data) {
        const auto& options=request["data"];
        valid(options.is_object() && options.size()==4 && options.contains("mode") && options["mode"].is_string() && options.contains("range") && options["range"].is_array() &&
            options.contains("invalid_values") && options["invalid_values"].is_string() && options.contains("policy") && options["policy"]=="v1");
        for(const auto& number:options["range"])valid(number.is_number());
        auto mapping=detail::data_mapping(options["mode"].get<std::string>(),options["range"].get<std::vector<double>>(),options["invalid_values"].get<std::string>());
        valid(mapping.mode!=detail::DataMode::colour);
        auto count=key["channels"].size();valid(mapping.mode==detail::DataMode::cryptomatte?(count>=2 && count%2==0):mapping.mode==detail::DataMode::normal || mapping.mode==detail::DataMode::id_rgb?count==3:mapping.mode==detail::DataMode::vector?(count==2 || count==3):count==1);
    }
    for(const char* field:{"part_name","channels","strategy"})valid(request.contains(field) && request[field].is_string());
    valid(request["strategy"]=="primary" || request["strategy"]=="root_rgb");
    valid(request.contains("part") && request["part"].is_number_integer());
    auto part=request["part"].get<long double>();valid(part>=-1 && part<=std::numeric_limits<int>::max());
    valid(part<0 || request["part_name"]=="");
    valid(request["part_name"]=="" || request["part_name"]==key["part_name"]);
    valid(request["channels"]=="" || request["channels"]==key["group"]);
    if(data)valid(request["channels"]!="");
    valid(named || part<=0);
    auto previous=sequence_lock(id);
    if(!previous.is_null()) {valid(previous==lock);return;}
    impl_->sequences.emplace(id,Impl::Pin{key,request,named});
}

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
        {"quantization","unorm8-half-up-v1"},
        {"png_encoding",{{"compression_levels",{{"fast",1},{"balanced",3},{"quality",6}}},{"filter","adaptive"}}},
#ifdef THUMBGEN_C2_PNG_STREAM
        {"colour_processing",{"reference","srgb8-linear-v1","integer-linear-v1","integer-linear-stream-v1"}},
#else
        {"colour_processing",{"reference","srgb8-linear-v1","integer-linear-v1"}},
#endif
        {"icc_engines",{"LittleCMS2 2.20","skcms"}},
        {"visualization_modes",{"colour","depth","normal","vector","id","id_rgb","cryptomatte"}},
        {"deep_modes",{"reject","flatten"}},
        {"deep_compressions",detail::bounded_deep_zstd_available()?Json::array({"none","rle","zips","zstd"}):Json::array({"none","rle","zips"})},
        {"libraries",OIIO::get_string_attribute("library_list")},
        {"output_formats",output_formats},{"presets",{"fast","balanced","quality"}},
        {"defaults",{{"format","jpeg"},{"quality",90},{"size",{1024,1024}},
                     {"background","black"},{"preset","balanced"}}},
        {"limitations",{"Deep flattening is opt-in for associated linear HALF/FLOAT colour with matching alpha and base-layer Z; compression support depends on the runtime Core capability",
            "Deep positive-infinity points are supported; finite-front infinite-back volumes are refused",
            "batch defaults to one worker; explicit execution budgets enable a bounded process pool",
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
        const auto crypto=detail::crypto_names(spec);if(!crypto.empty())value["cryptomatte_families"]=crypto;
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
    if(impl_->converting)throw std::logic_error("Engine conversion is not reentrant");
    struct Active {bool& value;explicit Active(bool& v):value(v){value=true;}~Active(){value=false;}} active{impl_->converting};
    auto started=Clock::now();
    Json r{{"schema_version",1},{"type","result"},{"id",job.id},{"input",job.input},
           {"ok",false},{"status","failed"},{"outputs",Json::array()},{"warnings",Json::array()}};
    if(!job.sequence_id.empty())r["sequence_id"]=job.sequence_id;
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
    detail::ExrProfile profile(r);
#endif
    std::string stage="open";
    try {
        detail::DataMapping mapping;
        try {mapping=detail::data_mapping(job.options.visualization,job.options.data_range,job.options.invalid_values);}
        catch(const std::invalid_argument& error){throw Failure("visualization_options_invalid",error.what());}
        const bool data=mapping.mode!=detail::DataMode::colour;
        const bool categorical=mapping.mode==detail::DataMode::id || mapping.mode==detail::DataMode::id_rgb;
        const bool crypto=mapping.mode==detail::DataMode::cryptomatte;
        if((job.options.deep_mode!="reject" && job.options.deep_mode!="flatten") || job.options.deep_samples_per_pixel<1 || job.options.deep_samples_per_pixel>1048576 || (data && job.options.deep_mode!="reject"))
            throw Failure("deep_options_invalid","invalid Deep options or non-colour visualization");
        if(data && (job.options.channels.empty() || job.options.input_space!="auto" || !job.options.ocio_config.empty() || !job.options.display.empty() || !job.options.view.empty()))
            throw Failure("visualization_options_invalid","data visualization needs explicit channels and does not accept colour transform options");
        OIIO::attribute("threads",job.options.threads);
        OIIO::attribute("exr_threads",job.options.threads);
        auto tick=Clock::now();
        const auto source_path=fs::u8path(job.input);
        detail::SourceStamp stamp;
        try {stamp=detail::source_stamp(source_path);}
        catch(const Failure& e) {
            std::error_code ec;
            if(!fs::exists(source_path,ec) && !ec)throw Failure("open_failed",e.what());
            throw;
        }
        r["source_check"]={{"method","identity_size_mtime"},{"checks",0},{"stable",true}};
        auto verify_source=[&]() {
            TG_EXR_TICK(source_check_tick);
            r["source_check"]["checks"]=r["source_check"]["checks"].get<int>()+1;
            if(!detail::source_matches(source_path,stamp)) {
                r["source_check"]["stable"]=false;
                throw Failure("source_changed","source identity, size or mtime changed during processing",true);
            }
            TG_EXR_TOCK(source_check_tick,"exr_source_verify_ms");
        };
        OIIO::ImageSpec read_config;
        // Ask unassociated formats to return straight RGB. In particular WebP
        // and PNG otherwise associate their nonlinear RGB in different spaces.
        read_config.attribute("oiio:UnassociatedAlpha",1);
        bool container_icc=false;
        try {container_icc=!data && detail::container_has_icc(job.input,job.options.input_space=="auto"?detail::icc_profile_limit:std::numeric_limits<std::size_t>::max());}
        catch(const detail::CmsFailure& error){throw Failure("colour_input_unsupported",error.what());}
        auto in=OIIO::ImageInput::open(job.input,&read_config);
        if(!in){auto error=OIIO::geterror();verify_source();throw Failure("open_failed",error);}
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
        profile.enable(std::string(in->format_name())=="openexr");
#endif
        if(!data && detail::container_separated_colour(job.input))
            throw Failure("colour_input_unsupported","CMYK/separated input requires an external profile-aware conversion to RGB");
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
        bool part_exists=false,has_deep=false,has_data=false,matched_group=false,crypto_metadata_seen=false;
        std::set<std::string> crypto_rank_groups;
        for(int p=0;;++p) {
            OIIO::ImageSpec candidate;
            if(!in->seek_subimage(p,0,candidate))break;
            if(!data && job.options.input_space=="auto" && p>0 && (container_icc || candidate.find_attribute("ICCProfile")))
                throw Failure("colour_input_unsupported","automatic ICC supports a single flat image, not multipart/multiframe input");
            const bool supported_cmyk=job.options.input_space=="auto" && candidate.find_attribute("ICCProfile") &&
                ((std::string(in->format_name())=="tiff" && candidate.get_string_attribute("tiff:ColorSpace")=="CMYK") ||
                 (std::string(in->format_name())=="jpeg" && !candidate.get_string_attribute("jpeg:ColorSpace").empty()));
            if(!data && detail::separated_colour(candidate) && !supported_cmyk)
                throw Failure("colour_input_unsupported","CMYK/separated input requires an external profile-aware conversion to RGB; input_space cannot undo decoder conversion");
            ++part_count;
            auto groups=candidate.deep && job.options.deep_mode=="flatten"?detail::deep_colour_candidates(candidate):detail::classify(candidate);
            if(crypto) {
                const std::vector<std::string> family{job.options.channels};
                auto record=[&](const std::string& group){if(group!=job.options.channels && detail::crypto_group(family,group))crypto_rank_groups.insert(group);};
                for(const auto& channel:candidate.channelnames){auto dot=channel.rfind('.');if(dot!=std::string::npos)record(channel.substr(0,dot));}
                record(std::string(candidate.get_string_attribute("oiio:subimagename")));
            }
            for(const auto& group:groups)r["selection"]["candidates"].push_back(detail::candidate_json(candidate,group,p));
            if(job.options.part>=0 && p!=job.options.part)continue;
            if(!job.options.part_name.empty() && candidate.get_string_attribute("oiio:subimagename")!=job.options.part_name)continue;
            part_exists=true;has_deep=has_deep || candidate.deep;
            if(crypto) {
                const auto names=detail::crypto_names(candidate);
                crypto_metadata_seen=crypto_metadata_seen || std::find(names.begin(),names.end(),job.options.channels)!=names.end();
                if(auto layout=detail::crypto_layout(candidate,job.options.channels)) {
                    detail::Candidate group{job.options.channels,"data","explicit Cryptomatte coverage overview",layout->channels};
                    all_choices.push_back({p,candidate,group});choices.push_back({p,candidate,std::move(group)});
                }
                continue;
            }
            std::vector<detail::Candidate> eligible;
            for(const auto& group:groups) {
                bool literal=data && std::any_of(group.channels.begin(),group.channels.end(),[&](int c){return candidate.channelnames[c]==job.options.channels;});
                if(!job.options.channels.empty() && group.group!=job.options.channels && !literal)continue;
                matched_group=true;has_data=has_data || group.kind=="data";
                if(data && group.kind!="deep") {
                    auto chosen=detail::data_candidate(candidate,group,job.options.channels,mapping);
                    eligible.push_back(chosen);all_choices.push_back({p,candidate,std::move(chosen)});
                } else if(!data && (group.kind=="colour" || group.kind=="grey")) {
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
        Json request=detail::selection_request(job);
        auto pin=impl_->sequences.find(job.sequence_id);
        auto key=[](const Choice& choice) {
            auto value=detail::candidate_json(choice.spec,choice.candidate,choice.part);
            // Named parts may move in the file without changing their identity.
            if(!value["part_name"].get<std::string>().empty())value.erase("part");
            value.erase("reason");value.erase("primary");
            if(choice.spec.deep) {
                try{value["deep"]=detail::deep_selection(choice.spec,choice.candidate).identity();}
                catch(const detail::DeepFailure&){value["deep"]=nullptr;}
            }
            return value;
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
            if(has_deep)throw Failure(job.options.deep_mode=="flatten"?"deep_layout_unsupported":"deep_requires_flattening","Deep data requires an explicit supported colour mapping and flatten mode");
            if(crypto)throw Failure(crypto_metadata_seen?"cryptomatte_layout_unsupported":"cryptomatte_metadata_invalid","requested Cryptomatte family requires metadata and complete same-part rank layers");
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
        std::optional<detail::DeepSelection> deep;
        if(spec.deep)deep=detail::deep_selection(spec,chosen.candidate);
        r["selection"]["chosen"]=detail::candidate_json(spec,chosen.candidate,part);
        if(spec.width<1 || spec.height<1 || spec.depth!=1 || spec.full_width<1 || spec.full_height<1)
            throw Failure("invalid_dimensions","unsupported image dimensions");
        auto crypto_layout=crypto?detail::crypto_layout(spec,job.options.channels):std::optional<detail::CryptoLayout>{};
        if(crypto_layout) {
            std::set<std::string> contained;for(const auto& layer:crypto_layout->layers)contained.insert(layer.name);
            if(!std::includes(contained.begin(),contained.end(),crypto_rank_groups.begin(),crypto_rank_groups.end()))
                throw Failure("cryptomatte_layout_unsupported","Cryptomatte ranks span parts; incomplete same-part preview is refused");
        }
        if(crypto_layout)r["cryptomatte"]=crypto_layout->info();
        if(!job.sequence_id.empty() && pin==impl_->sequences.end()) {
            bool named=!spec.get_string_attribute("oiio:subimagename").empty();
            if(!named && part_count!=1)throw Failure("sequence_part_identity_required","sequence selection requires unique named parts for multipart files");
            impl_->sequences.emplace(job.sequence_id,Impl::Pin{chosen_key,request,named});
            r["selection"]["lock_status"]="created";r["selection"]["locked"]=chosen_key;
        }
        r["source"]=spec_json(spec,part);
        if(deep)r["deep"]={{"mode","flatten"},{"policy","deep-colour-sweep-v1"},{"mapping",deep->identity()},
            {"sample_limit",job.options.deep_samples_per_pixel},{"buffer_limit_bytes",job.options.max_decode_mib*1024ULL*1024ULL},
            {"samples",0},{"chunks",0},{"peak_buffer_bytes",0}};
        r["selected_channels"]=Json::array();
        for(int c:channels)r["selected_channels"].push_back(spec.channelnames[c]);
        r["timings_ms"]["open_select"]=ms(tick);
        // Metadata snapshots can own independent ICC byte copies. Release
        // unselected candidates before admission and never propagate a source
        // profile onto the CMS/OCIO-converted RGB pixels.
        all_choices.clear();choices.clear();
        bool all_skip=!job.outputs.empty();
        for(const auto& output:job.outputs) {
            if(output.existing!="skip"){all_skip=false;break;}
            try {if(!detail::existing_target(fs::u8path(output.path),source_path)){all_skip=false;break;}}
            catch(const Failure&){all_skip=false;break;}
        }
        if(all_skip) {
            verify_source();
            in->close();
            for(const auto& output:job.outputs)r["outputs"].push_back({{"path",output.path},{"status","skipped"},{"reason","output_exists"},{"existing_policy","skip"}});
            r["status"]="skipped";r["ok"]=true;r["timings_ms"]["total"]=ms(started);return r;
        }
        auto input_plan=detail::channel_plan(spec,crypto?crypto_layout->layers.front().channels:channels,data);
        const bool cmyk=!data && detail::separated_colour(spec);
        const bool cmyk_jpeg=cmyk && std::string(in->format_name())=="jpeg";
        OIIO::ImageSpec raw_cmyk;
        if(cmyk) {
            raw_cmyk=spec;raw_cmyk.nchannels=4;raw_cmyk.channelnames={"C","M","Y","K"};raw_cmyk.alpha_channel=-1;
            raw_cmyk.erase_attribute("ICCProfile");
            input_plan=detail::channel_plan(raw_cmyk,{0,1,2,3},false);
        }
        const std::string input_format=in->format_name();
        const bool icc_format=input_format=="png" || input_format=="jpeg" || input_format=="tiff" || input_format=="webp";
        const bool use_icc=!data && !deep && icc_format && job.options.input_space=="auto" && (container_icc || spec.find_attribute("ICCProfile"));
        if(crypto)input_plan.pixel_bytes=detail::crypto_source_pixel_bytes(spec,*crypto_layout);
        std::optional<detail::SampledIdPlan> sampled;
        if(mapping.mode==detail::DataMode::id) {
            int dw=spec.full_width,dh=spec.full_height;auto orientation=spec.get_int_attribute("Orientation",1);
            if(orientation>=5 && orientation<=8)std::swap(dw,dh);
            std::vector<std::pair<int,int>> sizes;
            for(const auto& out:job.outputs) {
                double scale=std::min({double(out.width)/dw,double(out.height)/dh,1.0});
                sizes.emplace_back(std::max(1,int(std::lround(dw*scale))),std::max(1,int(std::lround(dh*scale))));
            }
            sampled=detail::sampled_id_plan(spec,in->format_name(),channels,mapping,sizes);
            if(sampled)r["source_read"]={{"mode","sampled_blocks"},{"grid",{sampled->x.size(),sampled->y.size()}},
                {"block",{sampled->block_width,sampled->block_height}},{"calls",sampled->x_blocks.size()*sampled->y_blocks.size()},{"buffer_bytes",sampled->peak_bytes}};
        }
        // Decide native storage from metadata before decode admission. The
        // pinned built-in colour contract is checked after admission, before IO.
        std::string requested_config=job.options.ocio_config;
        if(requested_config.empty()){const char* env=std::getenv("OCIO");requested_config=env?env:"";}
        if(requested_config.empty())requested_config="ocio://default";
        std::string requested_input=job.options.input_space;
        if(requested_input=="auto") {
            requested_input=spec.get_string_attribute("oiio:ColorSpace");
            if(requested_input.empty())requested_input="sRGB";
        }
        const bool plain_rgb=spec.nchannels==3 && spec.alpha_channel==-1 &&
            spec.channelnames==std::vector<std::string>({"R","G","B"}) && channels==std::vector<int>({0,1,2});
        const bool plain_rgba=spec.nchannels==4 && spec.alpha_channel==3 &&
            spec.get_int_attribute("oiio:UnassociatedAlpha",0)==1 &&
            spec.channelnames==std::vector<std::string>({"R","G","B","A"}) && channels==std::vector<int>({0,1,2,3});
        const auto colour_plan=detail::colour_plan(spec,input_format,
            !impl_->control.reference_colour && !data && !deep && !cmyk &&
            job.options.preset=="balanced" && requested_config=="ocio://default" && job.options.display.empty(),
            plain_rgb || plain_rgba,container_icc || spec.find_attribute("ICCProfile"),use_icc,requested_input);
        const bool fast_srgb=colour_plan.route==detail::ColourPlan::Route::srgb8;
        const bool integer_linear=colour_plan.route==detail::ColourPlan::Route::integer_linear;
#ifdef THUMBGEN_PNG_SCANLINE
        // Reject small sources before entering the metadata/header helper.
        // Positive int dimensions multiply safely after widening to uint64.
        const bool scanline_png=integer_linear && spec.width>0 && spec.height>0 &&
            std::uint64_t(spec.width)*std::uint64_t(spec.height)>=detail::png_scanline_min_pixels &&
            detail::png_scanline_eligible(source_path,spec,input_format,colour_plan);
#else
        const bool scanline_png=false;
#endif
        bool c2_active=false;
#ifdef THUMBGEN_C2_PNG_STREAM
        std::unique_ptr<detail::C2Resize> c2_resize;
        std::vector<std::pair<int,int>> c2_sizes;
        std::vector<OIIO::ImageBuf> c2_previews;
        std::vector<double> c2_resize_ms;
        // Explicit opt-in scope: integer-linear PNG and <=2 small downscaled outputs.
        // Execution and admission share the same bounded buffer envelope.
        const auto c2_decode_budget=job.options.max_decode_mib*1024ULL*1024ULL;
        const auto c2_cms_reserve=use_icc?detail::cms_reservation:0;
        std::uint64_t c2_dynamic_bytes=0;
        const bool c2_png=scanline_png || (integer_linear && spec.format==OIIO::TypeDesc::UINT8 &&
            std::uint64_t(spec.width)*spec.height>=detail::png_scanline_min_pixels && detail::png_scanline_eligible(source_path,spec,input_format,colour_plan,true));
        if(c2_png && spec.width<=65536 && spec.height<=65536 && !job.outputs.empty() && job.outputs.size()<=2) {
            bool c2_small_outputs=true;
            int c2_largest_w=0,c2_largest_h=0;
            for(const auto& output:job.outputs){
                double scale=std::min({double(output.width)/spec.width,double(output.height)/spec.height,1.0});
                int w=std::max(1,int(std::lround(spec.width*scale))),h=std::max(1,int(std::lround(spec.height*scale)));
                if(std::uint64_t(w)*h>960ULL*960 || w>=spec.width || h>=spec.height)c2_small_outputs=false;
                c2_sizes.emplace_back(w,h);
                if(std::uint64_t(w)*h>std::uint64_t(c2_largest_w)*c2_largest_h){c2_largest_w=w;c2_largest_h=h;}
            }
            if(c2_small_outputs){
                c2_dynamic_bytes=detail::c2_memory(spec.width,spec.height,spec.nchannels,c2_largest_w,c2_largest_h,c2_cms_reserve).dynamic();
                if(c2_dynamic_bytes<=c2_decode_budget){
                    try{
                        c2_resize=std::make_unique<detail::C2Resize>(spec.width,spec.height,spec.nchannels,c2_sizes);
                        c2_active=true;
                    }catch(const detail::C2PlanLimit&){c2_resize.reset();}
                }
            }
        }
#endif
        if(impl_->control.planned) {
            auto span=std::minmax_element(channels.begin(),channels.end());
            int display_width=spec.full_width,display_height=spec.full_height;
            auto orientation=spec.get_int_attribute("Orientation",1);
            if(orientation>=5 && orientation<=8)std::swap(display_width,display_height);
            int largest_w=1,largest_h=1;std::uint64_t largest_pixels=0;
            for(const auto& out:job.outputs) {
                double scale=std::min({double(out.width)/display_width,double(out.height)/display_height,1.0});
                int w=std::max(1,int(std::lround(display_width*scale))),h=std::max(1,int(std::lround(display_height*scale)));
                auto pixels=std::uint64_t(w)*std::uint64_t(h);
                if(pixels>largest_pixels){largest_pixels=pixels;largest_w=w;largest_h=h;}
            }
            Json plan{{"base",r},{"source_stamp",{{"identity",stamp.identity},{"size",stamp.size},
                {"modified_seconds",stamp.modified_seconds},{"modified_fraction",stamp.modified_fraction}}},
                {"footprint",{{"data_width",spec.width},{"data_height",spec.height},
                    {"display_width",display_width},{"display_height",display_height},
                    {"channel_span",cmyk?4:*span.second-*span.first+1},{"selected_channels",data || deep?4:channels.size()},
                    {"source_sample_bytes",colour_plan.linear()?colour_plan.sample_bytes:input_plan.sample_bytes},{"source_pixel_bytes",colour_plan.linear()?channels.size()*colour_plan.sample_bytes:input_plan.pixel_bytes},
                    {"source_buffer_bytes",sampled?sampled->peak_bytes:0},
                    {"deep_buffer_bytes",deep?job.options.max_decode_mib*1024ULL*1024ULL:0},
                    {"cms_buffer_bytes",use_icc?detail::cms_reservation:0},
                    {"native_categorical",categorical},{"srgb8",fast_srgb},{"integer_linear",integer_linear},{"c2_stream",c2_active},
                    {"output_width",largest_w},{"output_height",largest_h},{"threads",job.options.threads}}},
                {"sequence_pin",sequence_lock(job.sequence_id)}};
            impl_->control.planned(job,plan);
            verify_source();
        }
        TG_EXR_TICK(config_setup_tick);
        OIIO::ColorConfig* config=nullptr;
        std::string working_space,input_space,output_space;
        if(!data) {
            stage="colour";
            const bool embedded_icc=container_icc || spec.find_attribute("ICCProfile");
            if(embedded_icc && job.options.input_space=="auto" && !use_icc)
                throw Failure("colour_input_unsupported","ICC is unsupported on this input format or Deep input");
            if(embedded_icc && !use_icc)
                r["warnings"].push_back("embedded ICC profile bypassed by explicit input_space; no ICC transform or profile validation performed");
            std::string config_name=job.options.ocio_config,config_basis="explicit";
            if(config_name.empty()) {
                const char* environment=std::getenv("OCIO");
                config_name=environment?environment:"";config_basis="environment";
                if(config_name.empty()){config_name="ocio://default";config_basis="builtin";}
            }
            r["colour"]={{"ocio_config",config_name},{"config_basis",config_basis},
                {"input_requested",job.options.input_space},{"display",job.options.display},{"view",job.options.view}};
            TG_EXR_TICK(config_lookup_tick);
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
            const bool reused_config=impl_->configs.find(config_name)!=impl_->configs.end();
#endif
            config=impl_->config(config_name);
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
            profile.add(reused_config?"exr_ocio_config_reuse_ms":"exr_ocio_config_create_ms",config_lookup_tick);
#endif
            auto space=[&](const std::string& name) {
                if(config->getColorSpaceIndex(name)<0)
                    throw Failure("colour_space_not_found","colour space or role not found: "+name);
                return std::string(config->resolve(name));
            };
            const auto roles=config->getRoles();
            if(std::find(roles.begin(),roles.end(),"scene_linear")==roles.end())
                throw Failure("colour_working_space_missing","OCIO config must define the scene_linear role");
            working_space=space("scene_linear");
            if(!config->isColorSpaceLinear(working_space))
                throw Failure("colour_working_space_invalid","scene_linear role must use a linear colour space");
            std::string from=job.options.input_space,basis="explicit";
            if(use_icc) {
                from=std::string(config->resolve("srgb_rec709_scene"));basis="embedded_icc";
                // Custom configs must explicitly declare this standardized
                // scene sRGB bridge; arbitrary scene_linear is never relabeled.
                bool declared=config_name=="ocio://default";
                for(int i=0;i<config->getNumColorSpaces();++i)
                    declared=declared || std::string(config->getColorSpaceNameByIndex(i))=="srgb_rec709_scene";
                if(!declared || from.empty() || config->get_color_interop_id(from)!="srgb_rec709_scene")
                    throw Failure("colour_input_unsupported","ICC requires a declared standard srgb_rec709_scene OCIO bridge");
            } else if(from=="auto") {
                from=spec.get_string_attribute("oiio:ColorSpace");basis="metadata";
                if(from.empty()) {
                    auto ext=fs::u8path(job.input).extension().u8string();
                    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
                    from=ext==".exr"||ext==".hdr"?"linear":"sRGB";basis="format_assumption";
                    r["warnings"].push_back("input colour space assumed from format; override with input_space");
                }
            }
            input_space=space(from);
            if(deep && (!config->isColorSpaceLinear(input_space) || spec.get_int_attribute("oiio:UnassociatedAlpha",0)))
                throw Failure("deep_colour_unsupported","Deep flattening requires associated samples in a verified linear input space");
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
            // The standard scene-sRGB bridge has linear Rec.709 primaries.
            // Custom configs, cropped windows and non-colour inputs keep
            // the reference path. Selection is already settled above.
            if(colour_plan.linear()) {
                if(config_name!="ocio://default" || (!integer_linear && config->get_color_interop_id(input_space)!="srgb_rec709_scene") ||
                   config->get_color_interop_id(output_space)!="srgb_rec709_scene")
                    throw Failure("colour_failed","standard sRGB fast-path colour contract changed");
                bool found=false;
                for(int i=0;i<config->getNumColorSpaces();++i) {
                    std::string name(config->getColorSpaceNameByIndex(i));
                    if(config->get_color_interop_id(name)=="lin_rec709_scene" && config->isColorSpaceLinear(name)) {
                        working_space=name;found=true;break;
                    }
                }
                if(!found)throw Failure("colour_space_not_found","standard linear Rec.709 space is unavailable");
            }
            r["colour"]["working_space"]=working_space;
            r["colour"]["processing"]=c2_active?"integer-linear-stream-v1":colour_plan.processing();
            if(!colour_plan.fallback.empty())r["colour"]["fallback_reason"]=colour_plan.fallback;
            if(use_icc)r["colour"]["icc"]={{"engine","LittleCMS2 2.20"},{"intent","relative_colorimetric"},{"black_point_compensation",false},
                {"model",cmyk?"CMYK":chosen.candidate.kind=="grey"?"Gray":"RGB"},
                {"bridge","srgb_rec709_scene"},{"profile_limit_bytes",detail::icc_profile_limit},{"cms_limit_bytes",detail::cms_memory_limit},
                {"reservation_bytes",detail::cms_reservation},{"peak_bytes",0}};
        } else {
            r["colour"]={{"input_space","data"},{"input_requested",job.options.visualization},{"basis","data_visualization"},
                {"working_space","display-referred"},{"output_space","sRGB"},{"ocio_config",""},{"config_basis","none"},{"display",""},{"view",""}};
            r["visualization"]={{"mode",job.options.visualization},{"policy","v1"},{"range",mapping.mode==detail::DataMode::id || crypto?Json::array():Json::array({mapping.minimum,mapping.maximum})},
                {"invalid_values",job.options.invalid_values},{"invalid_pixels",0},{"clipped_components",0}};
        }
        TG_EXR_TOCK(config_setup_tick,"exr_ocio_setup_ms");
        OIIO::ImageBuf working;
        detail::DataImage data_image;
        std::vector<int> order;
        // ImageBuf may wrap caller-owned storage. Keep the colour source alive
        // throughout all outputs, as in the original colour conversion path.
        std::vector<float> pixels;
#if defined(THUMBGEN_EXR_NOZERO) && THUMBGEN_EXR_NOZERO
        // Same job-wide lifetime as the reference pixels vector: NOZERO does
        // not also test an early-free policy or an application-buffer view.
        std::unique_ptr<float[]> exr_pixels;
#endif
        detail::DeepImage deep_image;
        if(integer_linear) {
            stage="decode";tick=Clock::now();
            const std::size_t budget=job.options.max_decode_mib*1024ULL*1024ULL;
            const std::size_t reservation=use_icc?detail::cms_reservation:0;
            // Reserve native + complete linear image + possible orientation copy
            // before allocation. Reference ICC validation fits the same reserve.
            const std::size_t per_pixel=channels.size()*(colour_plan.sample_bytes+2*sizeof(float));
            std::size_t count=std::size_t(spec.width);
            if(!c2_active && (reservation>budget || count>(budget-reservation)/per_pixel/std::size_t(spec.height)))
                throw Failure("decode_limit_exceeded","native colour source and linear buffers exceed decode budget");
            count*=std::size_t(spec.height);
            if(use_icc) {
                const auto* profile=spec.find_attribute("ICCProfile");detail::CmsStats stats;
                try {{detail::CmsInput validate(profile->data(),std::size_t(profile->datasize()),detail::CmsModel::rgb,stats);}
                    if(stats.live)throw detail::CmsFailure("CMS allocation ledger did not return to zero");}
                catch(const detail::CmsFailure& e){throw Failure("colour_input_unsupported",e.what());}
                r["colour"]["icc"]["engine"]="skcms";r["colour"]["icc"]["peak_bytes"]=stats.peak;
            }
#ifdef THUMBGEN_C2_PNG_STREAM
            if(c2_active){
                order=channels;c2_previews.reserve(c2_sizes.size());std::vector<float*> pointers;
                for(auto [w,h]:c2_sizes){
                    auto linear=spec;linear.set_format(OIIO::TypeDesc::FLOAT);linear.erase_attribute("ICCProfile");linear.attribute("oiio:UnassociatedAlpha",0);
                    linear.x=linear.y=linear.z=linear.full_x=linear.full_y=linear.full_z=0;
                    linear.width=linear.full_width=w;linear.height=linear.full_height=h;linear.depth=linear.full_depth=1;
                    c2_previews.emplace_back(linear);
                    if(!c2_previews.back().localpixels())throw Failure("resize_failed","cannot allocate incremental preview");
                    pointers.push_back(static_cast<float*>(c2_previews.back().localpixels()));
                }
                double decode_ms=0,colour_ms=0;
                try{
                    detail::C2PngRows rows(spec.width,spec.height,spec.nchannels,colour_plan,
                        [&](int first,int last,void* data){auto start=Clock::now();bool ok=in->read_scanlines(part,0,first,last,0,0,spec.nchannels,spec.format,data);decode_ms+=ms(start);if(!ok)throw Failure("decode_failed",in->geterror());},
                        [&](const void* data,float* out,std::size_t n){auto start=Clock::now();colour_plan.transform(data,out,n,spec.nchannels);colour_ms+=ms(start);});
                    c2_resize_ms=c2_resize->execute([&](int y,float* data){rows.get(y,data);},pointers);rows.finish();
                }catch(const detail::CmsFailure& error){throw Failure("colour_input_unsupported",error.what());}
                in->close();if(impl_->observer)impl_->observer("decoded",job);verify_source();
                r["timings_ms"]["decode"]=decode_ms;r["timings_ms"]["input_colour"]=colour_ms;
                c2_resize.reset();
            }else
#endif
            if(scanline_png) {
                auto linear=spec;linear.set_format(OIIO::TypeDesc::FLOAT);linear.erase_attribute("ICCProfile");linear.attribute("oiio:UnassociatedAlpha",0);
                working.reset(linear);order=channels;
                if(!working.localpixels())throw Failure("decode_failed","cannot allocate linear colour image");
                double colour_ms=0;
                try {
                    const bool ok=detail::png_scanline_blocks(spec.width,spec.height,spec.nchannels,
                        [&](int ybegin,int yend,void* rows) {return in->read_scanlines(part,0,ybegin,yend,0,0,spec.nchannels,spec.format,rows);},
                        [&](const void* block,std::size_t base,std::size_t n) {
                            const auto start=Clock::now();
                            colour_plan.transform(block,static_cast<float*>(working.localpixels())+base*spec.nchannels,n,spec.nchannels);
                            colour_ms+=ms(start);
                        });
                    if(!ok)throw Failure("decode_failed",in->geterror());
                } catch(const detail::CmsFailure& e){throw Failure("colour_input_unsupported",e.what());}
                in->close();if(impl_->observer)impl_->observer("decoded",job);verify_source();
                r["timings_ms"]["decode"]=std::max(0.0,ms(tick)-colour_ms);
                r["timings_ms"]["input_colour"]=colour_ms;
            } else {
                std::vector<std::uint16_t> native16(colour_plan.sample_bytes==2?count*channels.size():0);
                std::vector<std::uint8_t> native8(colour_plan.sample_bytes==1?count*channels.size():0);
                void* native=colour_plan.sample_bytes==2?static_cast<void*>(native16.data()):static_cast<void*>(native8.data());
                if(!in->read_image(part,0,0,spec.nchannels,spec.format,native))throw Failure("decode_failed",in->geterror());
                in->close();if(impl_->observer)impl_->observer("decoded",job);verify_source();
                r["timings_ms"]["decode"]=ms(tick);tick=Clock::now();
                auto linear=spec;linear.set_format(OIIO::TypeDesc::FLOAT);linear.erase_attribute("ICCProfile");linear.attribute("oiio:UnassociatedAlpha",0);
                working.reset(linear);order=channels;
                if(!working.localpixels())throw Failure("decode_failed","cannot allocate linear colour image");
                try {colour_plan.transform(native,static_cast<float*>(working.localpixels()),count,spec.nchannels);}
                catch(const detail::CmsFailure& e){throw Failure("colour_input_unsupported",e.what());}
                r["timings_ms"]["input_colour"]=ms(tick);
            }
            r["decoded_mib"]=double(count*channels.size()*colour_plan.sample_bytes)/1048576;
        } else if(fast_srgb) {
            stage="decode";tick=Clock::now();
            const size_t count=size_t(spec.width)*size_t(spec.height)*channels.size();
            if(count>job.options.max_decode_mib*1024ULL*1024ULL)
                throw Failure("decode_limit_exceeded","decoded source buffer exceeds limit");
            auto native=spec;native.set_format(OIIO::TypeDesc::UINT8);
            native.erase_attribute("ICCProfile");native.attribute("oiio:UnassociatedAlpha",1);
            working.reset(native);
            if(!in->read_image(part,0,0,spec.nchannels,OIIO::TypeDesc::UINT8,working.localpixels()))
                throw Failure("decode_failed",in->geterror());
            in->close();if(impl_->observer)impl_->observer("decoded",job);verify_source();
            order=channels;r["decoded_mib"]=double(count)/1048576;
            r["timings_ms"]["decode"]=ms(tick);
        } else if(data) {
            stage="visualization";
            try {
                data_image=sampled?detail::read_sampled_id(*in,spec,part,*sampled,mapping,job.options.max_decode_mib*1024ULL*1024ULL):
                    crypto?detail::read_crypto_image(*in,spec,part,*crypto_layout,mapping.invalid,job.options.max_decode_mib*1024ULL*1024ULL,job.options.threads):
                    detail::read_data_image(*in,spec,part,channels,mapping,job.options.max_decode_mib*1024ULL*1024ULL,job.options.threads);
                working=std::move(data_image.image);order={0,1,2,3};
                r["decoded_mib"]=double(data_image.decoded_bytes)/1048576;
                r["timings_ms"]["decode"]=data_image.decode_ms;r["timings_ms"]["visualization"]=data_image.mapping_ms;
                r["visualization"]["invalid_pixels"]=data_image.statistics.invalid_pixels;r["visualization"]["clipped_components"]=data_image.statistics.clipped_components;
                if(data_image.statistics.invalid_pixels)r["warnings"].push_back("data visualization replaced "+std::to_string(data_image.statistics.invalid_pixels)+" invalid pixels using "+job.options.invalid_values);
            }catch(const detail::InvalidDataValues& error) {
                in->close();verify_source();r["visualization"]["invalid_pixels"]=error.statistics.invalid_pixels;
                r["visualization"]["clipped_components"]=error.statistics.clipped_components;
                throw Failure("data_invalid_value",error.what());
            }catch(...){in->close();verify_source();throw;}
            in->close();if(impl_->observer)impl_->observer("decoded",job);verify_source();
        } else if(deep) {
            stage="decode";tick=Clock::now();in->close();verify_source();
            detail::DeepReadOutput output;
            output.mode=job.options.background=="transparent"?detail::DeepOutputMode::associated_rgba:detail::DeepOutputMode::opaque_rgba;
            if(job.options.background=="white") {
                // White is specified in the working linear basis. Transform a
                // single opaque pixel back to source before Deep composition.
                OIIO::ImageSpec bs(1,1,3,OIIO::TypeDesc::FLOAT);OIIO::ImageBuf background(bs),converted;
                check(iba::fill(background,std::vector<float>{1,1,1}),background,"colour_failed");
                if(input_space!=working_space) {
                    check(iba::colorconvert(converted,background,working_space,input_space,false,"","",config),converted,"colour_failed");
                    background=std::move(converted);
                }
                float colour[3];background.getpixel(0,0,colour);for(int c=0;c<3;++c)output.background[c]=colour[c];
            }
            detail::DeepReadLimits limits;limits.bytes=job.options.max_decode_mib*1024ULL*1024ULL;limits.samples_per_pixel=job.options.deep_samples_per_pixel;
            deep_image=detail::read_deep_image(source_path,part,deep->channels,limits,[&](const char* phase){
                if(impl_->observer)impl_->observer(std::string("deep_")+phase,job);verify_source();
            },output);
            if(deep_image.x!=spec.x || deep_image.y!=spec.y || deep_image.width!=spec.width || deep_image.height!=spec.height)
                throw Failure("source_changed","Deep geometry changed after metadata preparation",true);
            if(impl_->observer)impl_->observer("decoded",job);verify_source();
            r["decoded_mib"]=double(deep_image.bytes)/1048576;
            r["deep"]["samples"]=deep_image.samples;r["deep"]["chunks"]=deep_image.chunks;r["deep"]["peak_buffer_bytes"]=deep_image.peak_accounted_bytes;
            r["timings_ms"]["decode"]=ms(tick);
            auto flat=spec;flat.deep=false;flat.nchannels=4;flat.format=OIIO::TypeDesc::FLOAT;flat.channelformats.clear();flat.channelnames={"R","G","B","A"};flat.alpha_channel=3;flat.z_channel=-1;
            flat.attribute("oiio:UnassociatedAlpha",0);
            working=OIIO::ImageBuf(flat,deep_image.pixels.get());order={0,1,2,3};
            if(job.options.background=="transparent")for(std::size_t p=0;p<deep_image.bytes/sizeof(float);p+=4)
                if(deep_image.pixels[p+3]==0 && (deep_image.pixels[p]!=0 || deep_image.pixels[p+1]!=0 || deep_image.pixels[p+2]!=0))
                    throw Failure("deep_transparency_unsupported","Straight PNG/WebP cannot represent zero-alpha Deep emission; choose an opaque background");
        } else {
            const size_t total_limit=job.options.max_decode_mib*1024ULL*1024ULL;
            if(use_icc && total_limit<detail::cms_reservation)throw Failure("decode_limit_exceeded","ICC reservation exceeds decode budget");
            const size_t limit=(total_limit-(use_icc?detail::cms_reservation:0))/sizeof(float);
            size_t count=size_t(spec.width);
            for(size_t factor:{size_t(spec.height),input_plan.channels.size()}) {
                if(factor==0 || count>limit/factor)throw Failure("decode_limit_exceeded","decoded source buffer exceeds limit");
                count*=factor;
            }
            stage="decode";tick=Clock::now();
            TG_EXR_TICK(allocate_tick);
            TG_EXR_MARK("exr_nozero_hit",0.0);
#if defined(THUMBGEN_EXR_NOZERO) && THUMBGEN_EXR_NOZERO
            // Only a single dense FLOAT read of a flat EXR may overwrite the
            // uninitialized allocation. No pack/colour code runs unless that
            // complete read succeeds. Sparse runs and all non-EXR routes keep
            // the initialized reference allocation. Keep this owner alive
            // through every output, just like the reference pixels vector.
            if(input_format=="openexr" && !use_icc && !cmyk && spec.depth==1 &&
               !spec.get_int_attribute("openexr:luminancechroma",0) && input_plan.runs.size()==1 &&
               count>=detail::exr_nozero_min_samples) {
                exr_pixels.reset(new float[count]); // deliberately no ()
                TG_EXR_MARK("exr_nozero_hit",1.0);
            } else pixels.resize(count);
            float* colour_pixels=exr_pixels?exr_pixels.get():pixels.data();
#else
            pixels.resize(count);
            float* colour_pixels=pixels.data();
#endif
            TG_EXR_TOCK(allocate_tick,"exr_allocate_ms");
            if(cmyk && !cmyk_jpeg) {
                in->close();read_config.attribute("oiio:RawColor",1);in=OIIO::ImageInput::open(job.input,&read_config);
                if(!in || !in->seek_subimage(part,0,raw_cmyk) || raw_cmyk.nchannels!=4 || raw_cmyk.width!=spec.width || raw_cmyk.height!=spec.height ||
                   raw_cmyk.channelnames!=std::vector<std::string>{"C","M","Y","K"})
                    throw Failure("colour_input_unsupported","cannot read unmodified CMYK TIFF ink channels");
                raw_cmyk.erase_attribute("ICCProfile");
            }
            TG_EXR_TICK(read_tick);
            bool read_ok=false;
            if(cmyk_jpeg) {
                in->close();
                try{detail::read_cmyk_jpeg(job.input,spec.width,spec.height,colour_pixels);read_ok=true;}
                catch(const detail::CmsFailure& error){throw Failure("colour_input_unsupported",error.what());}
            } else read_ok=detail::read_selected(*in,cmyk?raw_cmyk:spec,part,input_plan,false,colour_pixels);
            TG_EXR_TOCK(read_tick,"exr_read_selected_ms");
            const std::string read_error=read_ok?std::string{}:in->geterror();
            TG_EXR_TICK(close_tick);
            in->close();
            TG_EXR_TOCK(close_tick,"exr_input_close_ms");
            if(impl_->observer)impl_->observer("decoded",job);
            verify_source();
            if(!read_ok)throw Failure("decode_failed",read_error);
            r["timings_ms"]["decode"]=ms(tick);
            r["decoded_mib"]=double(count*sizeof(float))/1048576;
            auto apply_icc=[&](float* values,int components,detail::CmsModel model,bool associated) {
                auto* profile=spec.find_attribute("ICCProfile");
                if(!profile || profile->type().basetype!=OIIO::TypeDesc::UINT8 || profile->datasize()<=0)
                    throw Failure("colour_input_unsupported","embedded ICC was discarded or is invalid");
                detail::CmsStats stats;
                try {
                    {detail::CmsInput cms(profile->data(),std::size_t(profile->datasize()),model,stats);
                     cms.apply(values,std::size_t(spec.width)*spec.height,components,associated);}
                    if(stats.live)throw detail::CmsFailure("CMS allocation ledger did not return to zero");
                } catch(const detail::CmsFailure& error){throw Failure("colour_input_unsupported",error.what());}
                r["colour"]["icc"]["peak_bytes"]=stats.peak;
            };
            if(cmyk)apply_icc(colour_pixels,4,detail::CmsModel::cmyk,false);
            TG_EXR_TICK(pack_tick);
            OIIO::ImageSpec subset=spec;
            subset.erase_attribute("ICCProfile");
            subset.nchannels=int(input_plan.channels.size());subset.format=OIIO::TypeDesc::FLOAT;subset.channelformats.clear();
            subset.channelnames.clear();for(int c:input_plan.channels)subset.channelnames.push_back(cmyk?std::vector<std::string>{"R","G","B","A"}[c]:spec.channelnames[c]);
            auto alpha=std::find(input_plan.channels.begin(),input_plan.channels.end(),spec.alpha_channel);
            subset.alpha_channel=alpha!=input_plan.channels.end()?int(alpha-input_plan.channels.begin()):-1;
            auto depth=std::find(input_plan.channels.begin(),input_plan.channels.end(),spec.z_channel);
            subset.z_channel=depth!=input_plan.channels.end()?int(depth-input_plan.channels.begin()):-1;
            OIIO::ImageBuf buffer(subset,colour_pixels);
            for(int c:channels)order.push_back(int(std::lower_bound(input_plan.channels.begin(),input_plan.channels.end(),c)-input_plan.channels.begin()));
            std::vector<std::string> names{"R","G","B"};if(order.size()==4)names.push_back("A");
            check(iba::channels(working,buffer,int(order.size()),order,{},names),working,"channels_failed");
            if(use_icc && !cmyk)apply_icc(static_cast<float*>(working.localpixels()),working.nchannels(),
                chosen.candidate.kind=="grey"?detail::CmsModel::grey:detail::CmsModel::rgb,!spec.get_int_attribute("oiio:UnassociatedAlpha",0));
            if(spec.get_int_attribute("oiio:UnassociatedAlpha",0) && order.size()==4)
                check(iba::premult(working,working),working,"alpha_failed");
            working.specmod().attribute("oiio:UnassociatedAlpha",0);
            TG_EXR_TOCK(pack_tick,"exr_pack_alpha_ms");
        }
        TG_EXR_TICK(canvas_tick);
        const size_t limit=job.options.max_decode_mib*1024ULL*1024ULL/sizeof(float);
        // resize() clamps outside the data window. Pad a cropped display window
        // with zero pixels first, rather than stretching edge pixels into it.
        if(!categorical && !c2_active) {
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
        }
        // Orient a complete, zero-origin display canvas. Some OIIO rotations
        // position cropped data incorrectly when the full window has an origin.
        if(!categorical && !c2_active){working.set_origin(0,0,0);working.set_roi_full(working.roi());}
        if(!categorical && spec.get_int_attribute("Orientation",1)!=1) {
            OIIO::ImageBuf oriented;
            check(iba::reorient(oriented,working),oriented,"orientation_failed");working=std::move(oriented);
        }
        TG_EXR_TOCK(canvas_tick,"exr_canvas_orientation_ms");
        stage="colour";tick=Clock::now();
        TG_EXR_TICK(working_tick);
        if(!data && !colour_plan.linear() && input_space!=working_space)
            check(iba::colorconvert(working,working,input_space,working_space,true,"","",config),working,"colour_failed");
        TG_EXR_TOCK(working_tick,"exr_ocio_working_apply_ms");
        r["timings_ms"]["working_colour"]=ms(tick);
        const std::string filter=categorical?"nearest":job.options.preset=="fast"?"box":job.options.preset=="quality"?"lanczos3":"triangle";
        r["preset"]=job.options.preset;r["filter"]=filter;
        for(size_t output_index=0;output_index<job.outputs.size();++output_index) {
            const auto& out=job.outputs[output_index];
            Json result{{"path",out.path},{"status","failed"},{"existing_policy",out.existing}};
            std::unique_ptr<detail::TemporaryOutput> temp;
            try {
                fs::path dest=fs::u8path(out.path);
                if(out.existing!="error" && out.existing!="skip" && out.existing!="replace")throw Failure("invalid_output_policy","invalid existing output policy");
                bool exists=detail::existing_target(dest,source_path);
                if(exists && out.existing=="error")throw Failure("output_exists","output already exists");
                if(exists && out.existing=="skip") {
                    verify_source();result["status"]="skipped";result["reason"]="output_exists";
                    r["outputs"].push_back(result);continue;
                }
                verify_source();
                int display_width=(categorical || c2_active)?spec.full_width:working.spec().full_width;
                int display_height=(categorical || c2_active)?spec.full_height:working.spec().full_height;
                if(categorical && spec.get_int_attribute("Orientation",1)>=5 && spec.get_int_attribute("Orientation",1)<=8)std::swap(display_width,display_height);
                double scale=std::min({double(out.width)/display_width,double(out.height)/display_height,1.0});
                int w=std::max(1,int(std::lround(display_width*scale))),h=std::max(1,int(std::lround(display_height*scale)));
                OIIO::ImageBuf preview;
                tick=Clock::now();
                TG_EXR_TICK(resize_tick);
#ifdef THUMBGEN_C2_PNG_STREAM
                if(c2_active)preview=std::move(c2_previews[output_index]);
                else
#endif
                if(fast_srgb)
                    preview=detail::resize_srgb8(working,w,h);
                else if(integer_linear)
                    preview=detail::resize_linear(working,w,h);
                else if(crypto && w==display_width && h==display_height)
                    check(preview.copy(working),preview,"resize_failed");
                else if(categorical)
                    preview=detail::categorical_preview(data_image,w,h,job.options.max_decode_mib*1024ULL*1024ULL,job.options.threads);
                else check(iba::resize(preview,working,{{"filtername",filter}},OIIO::ROI(0,w,0,h,0,1,0,int(order.size())),job.options.threads),preview,"resize_failed");
                TG_EXR_TOCK(resize_tick,"exr_resize_sum_ms");
                result["resize_ms"]=ms(tick);tick=Clock::now();
#ifdef THUMBGEN_C2_PNG_STREAM
                if(c2_active)result["resize_ms"]=c2_resize_ms[output_index];
#endif
                TG_EXR_TICK(finish_tick);
                if(job.options.background!="transparent" && preview.nchannels()==4) {
                    OIIO::ImageSpec bs(w,h,4,OIIO::TypeDesc::FLOAT);bs.alpha_channel=3;
                    OIIO::ImageBuf background(bs),composite;
                    float value=job.options.background=="white"?1.0f:0.0f;
                    std::vector<float> colour{value,value,value,1.0f};
                    check(iba::fill(background,colour),background,"alpha_failed");
                    check(iba::over(composite,preview,background),composite,"alpha_failed");preview=std::move(composite);
                    // A fully opaque background has alpha=1 even when a
                    // negative-lobe filter produced alpha outside [0,1].
                    // Do not let that residual alpha reweight the subsequent
                    // colour transform before RGB output discards it.
                    check(iba::fill(preview,colour,OIIO::ROI(0,w,0,h,0,1,3,4),job.options.threads),preview,"alpha_failed");
                }
                TG_EXR_TICK(output_colour_tick);
                if(!data) {
                    if(job.options.display.empty())
                        check(iba::colorconvert(preview,preview,working_space,output_space,true,"","",config),preview,"colour_failed");
                    else
                        check(iba::ociodisplay(preview,preview,job.options.display,job.options.view,working_space,"",true,false,"","",config),preview,"colour_failed");
                }
                TG_EXR_TOCK(output_colour_tick,"exr_ocio_output_apply_sum_ms");
                if(job.options.background=="transparent" && preview.nchannels()==4) {
                    if(deep) {
                        const auto* p=static_cast<const float*>(preview.localpixels());
                        if(!p || !preview.contiguous())throw Failure("deep_transparency_unsupported","Deep transparent preview must be contiguous RGBA");
                        for(std::size_t i=0;i<std::size_t(w)*h*4;i+=4)
                            if(p[i+3]==0 && (p[i]!=0 || p[i+1]!=0 || p[i+2]!=0))
                                throw Failure("deep_transparency_unsupported","Straight PNG/WebP cannot represent zero-alpha Deep emission after filtering/colour conversion");
                    }
                    // Supply straight display RGB explicitly. PNG and WebP plugins
                    // otherwise deassociate in different spaces (WebP uses gamma 2.2).
                    check(iba::unpremult(preview,preview),preview,"alpha_failed");
                    preview.specmod().attribute("oiio:UnassociatedAlpha",1);
                }
                if(job.options.background!="transparent" || out.format=="jpeg") {
                    OIIO::ImageBuf rgb;std::vector<int> rgb_order{0,1,2};
                    check(iba::channels(rgb,preview,3,rgb_order),rgb,"channels_failed");preview=std::move(rgb);
                }
                // Processing preserves source metadata. A source compression
                // (EXR zip, TIFF lzw, WebP lossless, or codec:quality) must not
                // override the requested output codec and quality.
                preview.specmod().erase_attribute("compression");
                if(out.format=="png") {
                    preview.specmod().attribute("png:compressionLevel",job.options.preset=="fast"?1:job.options.preset=="quality"?6:3);
                    preview.specmod().attribute("png:filter",248); // PNG_ALL_FILTERS, explicit adaptive filtering.
                }
                if(out.format=="jpeg" || out.format=="webp")
                    preview.specmod().attribute("compression",out.format+":"+std::to_string(out.quality));
                preview.specmod().attribute("CompressionQuality",out.quality);
                preview.specmod().attribute("oiio:ColorSpace",job.options.display.empty()?"sRGB":"display-referred");
                TG_EXR_TICK(reserve_tick);
                fs::path temporary;
                if(impl_->control.reserve) {
                    temporary=fs::u8path(impl_->control.reserve(job,output_index));
                    if(temporary.empty() || !temporary.is_absolute())throw Failure("temporary_create_failed","controller must reserve an absolute temporary path");
                    auto a=fs::weakly_canonical(temporary),b=fs::weakly_canonical(dest);
#ifdef _WIN32
                    bool same=CompareStringOrdinal(a.c_str(),int(a.native().size()),b.c_str(),int(b.native().size()),TRUE)==CSTR_EQUAL;
#else
                    bool same=a==b;
#endif
                    if(same)throw Failure("temporary_create_failed","controller temporary path aliases the final output");
                    if(detail::existing_target(temporary,source_path))throw Failure("output_exists","reserved temporary path already exists");
                } else {
                    temp=std::make_unique<detail::TemporaryOutput>(dest,out.format);temporary=temp->path();
                }
                TG_EXR_TOCK(reserve_tick,"exr_output_reserve_sum_ms");
                write_preview(preview,temporary,job.options.threads,
                    !data && job.options.background=="transparent" && preview.nchannels()==4 TG_EXR_PROFILE_ARG);
                if(impl_->observer)impl_->observer("encoded",job);
                verify_source();
                TG_EXR_TICK(commit_tick);
                bool committed;
                if(impl_->control.commit) {
                    auto encoded=result;encoded["status"]="success";encoded["width"]=w;encoded["height"]=h;
                    encoded["format"]=out.format;encoded["finish_ms"]=ms(tick);
                    committed=impl_->control.commit(job,output_index,temporary.u8string(),{{"base",r},{"output",encoded}});
                    if(!committed && out.existing!="skip")throw Failure("commit_failed","controller returned skip for a non-skip policy");
                } else committed=temp->commit(dest,source_path,out.existing);
                TG_EXR_TOCK(commit_tick,"exr_output_commit_sum_ms");
                TG_EXR_TOCK(finish_tick,"exr_finish_sum_ms");
                if(committed) {
                    result["status"]="success";result["width"]=w;result["height"]=h;
                    result["finish_ms"]=ms(tick);result["format"]=out.format;
                } else {result["status"]="skipped";result["reason"]="output_exists";}
            }catch(const Failure& e){
                if(e.code=="source_changed")r["source_check"]["stable"]=false;
                result["error"]={{"code",e.code},{"message",e.what()},{"retryable",e.retryable}};
            }
             catch(const fs::filesystem_error& e){result["error"]={{"code","output_failed"},{"message",detail::error_message(e.code())}};}
             catch(const std::exception& e){result["error"]={{"code","output_failed"},{"message",e.what()}};}
            if(temp) {
                auto error=temp->cleanup();
                if(!error.empty())r["warnings"].push_back("temporary cleanup failed: "+temp->path().u8string()+": "+error);
            }
            r["outputs"].push_back(result);
        }
        size_t successes=0,skips=0;for(const auto& output:r["outputs"]) {
            if(output["status"]=="success")++successes;
            if(output["status"]=="skipped")++skips;
        }
        r["ok"]=successes+skips==job.outputs.size();
        r["status"]=skips==job.outputs.size()?"skipped":r["ok"].get<bool>()?"success":successes+skips?"partial":"failed";
    }catch(const detail::DeepFailure& e){
        r["error"]={{"code",e.code},{"message",e.what()},{"stage",stage},{"retryable",false}};
    }catch(const Failure& e){
        if(e.code=="source_changed" && r.contains("source_check"))r["source_check"]["stable"]=false;
        r["error"]={{"code",e.code},{"message",e.what()},{"stage",stage},{"retryable",e.retryable}};
    }
     catch(const std::exception& e){r["error"]={{"code","conversion_failed"},{"message",e.what()},{"stage",stage}};}
    r["timings_ms"]["total"]=ms(started);
    return r;
}
}
