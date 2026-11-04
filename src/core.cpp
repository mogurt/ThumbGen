#include <thumbgen/core.h>
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
static std::vector<int> select(const OIIO::ImageSpec& s, const std::string& group) {
    auto index = [&](const std::string& n) {
        auto it=std::find(s.channelnames.begin(),s.channelnames.end(),n);
        return it==s.channelnames.end() ? -1 : int(it-s.channelnames.begin());
    };
    auto rgb = [&](const std::string& prefix) {
        std::vector<int> out{index(prefix+"R"),index(prefix+"G"),index(prefix+"B")};
        if (*std::min_element(out.begin(),out.end())<0) return std::vector<int>{};
        int alpha=index(prefix+"A");
        if(alpha<0)alpha=index("A");
        if(alpha>=0)out.push_back(alpha);
        return out;
    };
    if(!group.empty()) {
        auto out=rgb(group=="RGB" ? "" : group+".");
        if(out.empty())throw Failure("channels_not_found","requested RGB channel group does not exist");
        return out;
    }
    auto root=rgb("");
    if(!root.empty())return root;
    std::vector<std::vector<int>> candidates;
    bool crypto=false;
    for(const auto& name:s.channelnames) {
        crypto=crypto || name.find("Crypto")==0 || name.find("crypto")==0;
        if(name.size()>2 && name.substr(name.size()-2)==".R") {
            auto out=rgb(name.substr(0,name.size()-1));
            if(!out.empty())candidates.push_back(out);
        }
    }
    if(crypto)throw Failure("visualization_required","Cryptomatte data requires dedicated visualization");
    if(candidates.size()==1)return candidates[0];
    if(s.nchannels==1 || (s.nchannels==2 && s.alpha_channel==1)) {
        std::vector<int> out{0,0,0};
        if(s.alpha_channel==1)out.push_back(1);
        return out;
    }
    throw Failure("ambiguous_channels","specify --channels for ambiguous or non-colour channels");
}

struct Engine::Impl {
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
        {"oiio_version",OIIO_VERSION_STRING},{"input_plugins",OIIO::get_string_attribute("format_list")},
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
    for(int part=0;in->seek_subimage(part,0,spec);++part)result["parts"].push_back(spec_json(spec,part));
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
        std::string selection_error;
        std::string selection_code="selection_failed";
        for(int p=0;;++p) {
            OIIO::ImageSpec candidate;
            if(!in->seek_subimage(p,0,candidate))break;
            if(job.options.part>=0 && p!=job.options.part)continue;
            if(candidate.deep){selection_error="Deep data requires flattening";selection_code="deep_requires_flattening";continue;}
            try {
                auto found=select(candidate,job.options.channels);
                if(part>=0)throw Failure("ambiguous_parts","multiple colour parts; specify --part");
                channels=std::move(found);spec=candidate;part=p;
                if(job.options.part>=0)break;
            }
            catch(const Failure& e) {
                if(job.options.part>=0 || e.code=="ambiguous_parts")throw;
                selection_error=e.what();selection_code=e.code;
            }
        }
        if(part<0)throw Failure(selection_code,selection_error.empty()?"no suitable part found":selection_error);
        if(spec.width<1 || spec.height<1 || spec.depth!=1 || spec.full_width<1 || spec.full_height<1)
            throw Failure("invalid_dimensions","unsupported image dimensions");
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
