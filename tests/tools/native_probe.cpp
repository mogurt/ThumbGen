// Archived selection prototype; not the public product CLI.
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace iba = OIIO::ImageBufAlgo;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string manifest, input, output, filter = "box", colour = "auto";
    int threads = 4;
    bool inspect = false, read_all = false;
    size_t max_read_mib = 512;
};

static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error.empty() ? "image operation failed" : error);
}
static void check_image(bool ok, const OIIO::ImageBuf& image) {
    if (!ok) require(false,image.geterror());
}
static std::vector<int> rgb_channels(const OIIO::ImageSpec& s) {
    const auto& names = s.channelnames;
    auto lookup = [&](const std::string& n) {
        auto it = std::find(names.begin(), names.end(), n);
        return it == names.end() ? -1 : int(it - names.begin());
    };
    std::vector<std::vector<int>> groups;
    for (const auto& n : names) {
        if (n == "R" || (n.size() > 2 && n.substr(n.size()-2) == ".R")) {
            std::string prefix = n.substr(0, n.size()-1);
            int r = lookup(prefix + "R"), g = lookup(prefix + "G"), b = lookup(prefix + "B");
            if (r < 0 || g < 0 || b < 0) continue;
            std::vector<int> group{r,g,b};
            int a = lookup(prefix + "A");
            if (a < 0) a = lookup("A");
            if (a >= 0) group.push_back(a);
            if (prefix.empty()) return group;
            groups.push_back(group);
        }
    }
    // A single named colour group is unambiguous; multiple groups require policy.
    if (groups.size() == 1) return groups.front();
    if (s.nchannels == 1 || (s.nchannels == 2 && s.alpha_channel == 1)) {
        std::vector<int> group{0,0,0};
        if (s.alpha_channel == 1) group.push_back(1);
        return group;
    }
    throw std::runtime_error("ambiguous or missing RGB channel group; explicit selection needed");
}

static json process(const json& item, const Options& opt) {
    const auto started = Clock::now();
    json result{{"id", item.at("id")}, {"path", item.at("path")}, {"ok", false}};
    result["filter"] = opt.filter;
    std::string stage = "open";
    try {
        const auto path = item.at("path").get<std::string>();
        auto tick = Clock::now();
        auto in = OIIO::ImageInput::open(path);
        require(bool(in), OIIO::geterror());
        json parts = json::array();
        OIIO::ImageSpec chosen;
        std::vector<int> selected;
        int chosen_part = -1;
        for (int part = 0; ; ++part) {
            OIIO::ImageSpec spec;
            if (!in->seek_subimage(part, 0, spec)) break;
            parts.push_back({{"part",part}, {"name",spec.get_string_attribute("oiio:subimagename")},
                {"width",spec.width}, {"height",spec.height}, {"channels",spec.channelnames},
                {"format",spec.format.c_str()}, {"deep",spec.deep},
                {"compression",spec.get_string_attribute("compression")},
                {"data_window",{spec.x,spec.y,spec.width,spec.height}},
                {"display_window",{spec.full_x,spec.full_y,spec.full_width,spec.full_height}},
                {"colour_metadata",spec.get_string_attribute("oiio:ColorSpace")}});
            if (chosen_part < 0 && !spec.deep) {
                try { selected = rgb_channels(spec); chosen = spec; chosen_part = part; }
                catch (const std::exception&) { /* keep inspecting other parts */ }
            }
        }
        result["parts"] = std::move(parts);
        result["open_ms"] = elapsed(tick);
        bool crypto=false, deep=false;
        for (const auto& part : result["parts"]) {
            deep = deep || part["deep"].get<bool>();
            for (const auto& channel : part["channels"]) {
                auto name=channel.get<std::string>();
                std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
                crypto = crypto || name.find("crypto")==0;
            }
        }
        result["classification"] = chosen_part>=0 ? "flat_colour" : crypto ? "cryptomatte_data"
            : deep ? "deep" : "ambiguous_or_noncolour";
        if (opt.inspect) {
            result["ok"] = true;
            result["selected_part"] = chosen_part;
            result["total_ms"] = elapsed(started);
            return result;
        }
        stage="selection";
        if (chosen_part<0) {
            result["error_code"] = crypto ? "cryptomatte_requires_visualization" : deep
                ? "deep_requires_flatten_policy" : "ambiguous_rgb_channels";
            throw std::runtime_error(crypto ? "Cryptomatte ID data requires a dedicated visualization policy"
                : deep ? "Deep image requires an explicit flatten policy"
                : "no unambiguous RGB group; explicit channel selection needed");
        }
        require(chosen.depth == 1 && chosen.width > 0 && chosen.height > 0,
                "invalid or volumetric image dimensions");
        require(chosen.full_width > 0 && chosen.full_height > 0, "invalid display window");
        result["selected_part"] = chosen_part;
        result["selected_channels"] = json::array();
        for (int c : selected) result["selected_channels"].push_back(chosen.channelnames[c]);
        auto bounds = std::minmax_element(selected.begin(), selected.end());
        int first = opt.read_all ? 0 : *bounds.first;
        int end = opt.read_all ? chosen.nchannels : *bounds.second + 1;
        const size_t count = size_t(chosen.width) * size_t(chosen.height) * size_t(end-first);
        require(count <= opt.max_read_mib * 1024ULL * 1024ULL / sizeof(float),
                "decoded channel buffer exceeds --max-read-mib");
        result["read_channels"] = end-first;
        result["pixel_buffer_mib"] = double(count*sizeof(float))/1048576;
        stage = "read"; tick = Clock::now();
        std::vector<float> pixels(count);
        const bool read_ok=in->read_image(chosen_part,0,first,end,OIIO::TypeDesc::FLOAT,pixels.data());
        require(read_ok,in->geterror());
        in->close();
        result["read_ms"] = elapsed(tick);
        OIIO::ImageSpec spec = chosen;
        spec.nchannels = end-first;
        spec.format = OIIO::TypeDesc::FLOAT;
        spec.channelformats.clear();
        spec.channelnames.assign(chosen.channelnames.begin()+first,chosen.channelnames.begin()+end);
        spec.alpha_channel = chosen.alpha_channel >= first && chosen.alpha_channel < end
            ? chosen.alpha_channel-first : -1;
        OIIO::ImageBuf source(spec,pixels.data()), rgba;
        std::vector<int> order;
        for (int c : selected) order.push_back(c-first);
        std::vector<std::string> names{"R","G","B"};
        if (selected.size() == 4) names.push_back("A");
        stage = "channels"; tick = Clock::now();
        check_image(iba::channels(rgba,source,int(order.size()),order,{},names),rgba);
        if (chosen.get_int_attribute("oiio:UnassociatedAlpha",0) && order.size()==4)
            check_image(iba::premult(rgba,rgba),rgba);
        if (chosen.get_int_attribute("Orientation",1)!=1) {
            OIIO::ImageBuf oriented;
            check_image(iba::reorient(oriented,rgba),oriented);
            rgba = std::move(oriented);
        }
        result["select_ms"] = elapsed(tick);
        std::string ext = fs::u8path(path).extension().u8string();
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
        std::string from = opt.colour == "auto"
            ? ((ext==".exr" || ext==".hdr") ? "linear" : "sRGB") : opt.colour;
        result["colour_policy"] = from=="raw" ? "raw" : from+" -> sRGB (assumed, reference pending)";
        result["alpha_policy"] = "premultiplied RGB over black";
        stage = "output";
        fs::create_directories(fs::u8path(opt.output));
        result["outputs"] = json::array();
        // Materialize one source; derive each output directly without intermediate JPEG.
        for (auto box : {std::pair<int,int>{960,960},{425,260}}) {
            const auto& rs = rgba.spec();
            double scale = std::min({double(box.first)/rs.full_width,double(box.second)/rs.full_height,1.0});
            int w=std::max(1,int(std::lround(rs.full_width*scale)));
            int h=std::max(1,int(std::lround(rs.full_height*scale)));
            OIIO::ImageBuf preview;
            tick = Clock::now();
            check_image(iba::resize(preview,rgba,{{"filtername",opt.filter}},
                                OIIO::ROI(0,w,0,h,0,1,0,int(order.size())),opt.threads),preview);
            double resize_ms=elapsed(tick);
            tick=Clock::now();
            if (from!="raw" && from!="sRGB")
                check_image(iba::colorconvert(preview,preview,from,"sRGB",true),preview);
            OIIO::ImageBuf rgb;
            std::vector<int> rgb_order{0,1,2};
            check_image(iba::channels(rgb,preview,3,rgb_order),rgb);
            double colour_ms=elapsed(tick);
            rgb.specmod().attribute("CompressionQuality",90);
            rgb.specmod().attribute("oiio:ColorSpace","sRGB");
            // The runner supplies a unique SHA256 id; validate even on untrusted manifests.
            const auto id=item.at("id").get<std::string>();
            require(!id.empty() && id.find_first_not_of("0123456789abcdef") == std::string::npos,
                    "id must be hexadecimal");
            fs::path dest=fs::u8path(opt.output)/(id+"-"+std::to_string(box.first)+".jpg");
            fs::path temp=dest; temp += ".partial.jpg";
            tick=Clock::now();
            check_image(rgb.write(temp.u8string(),OIIO::TypeDesc::UINT8),rgb);
            // Runner uses fresh run directories; do not replace existing outputs silently.
            require(!fs::exists(dest),"output already exists; use a fresh output directory");
            fs::rename(temp,dest);
            result["outputs"].push_back({{"path",dest.u8string()},{"width",w},{"height",h},
                {"resize_ms",resize_ms},{"colour_ms",colour_ms},{"write_ms",elapsed(tick)}});
        }
        result["ok"]=true;
    } catch(const std::exception& exc) {
        result["stage"]=stage;
        result["error"]=exc.what();
    }
    result["total_ms"]=elapsed(started);
    return result;
}

static int run(const std::vector<std::string>& argv) {
    Options opt;
    for (size_t i=1;i<argv.size();++i) {
        auto value=[&](){if(i+1>=argv.size())throw std::runtime_error("missing option value");return argv[++i];};
        const auto& a=argv[i];
        if(a=="--version") { std::cout<<json({{"tool","thumbgen-prototype"},{"oiio",OIIO_VERSION_STRING}}).dump()<<'\n';return 0; }
        if(a=="--manifest")opt.manifest=value();
        else if(a=="--input")opt.input=value();
        else if(a=="--output")opt.output=value();
        else if(a=="--filter")opt.filter=value();
        else if(a=="--colour")opt.colour=value();
        else if(a=="--threads")opt.threads=std::stoi(value());
        else if(a=="--max-read-mib")opt.max_read_mib=std::stoull(value());
        else if(a=="--inspect")opt.inspect=true;
        else if(a=="--read-all")opt.read_all=true;
        else throw std::runtime_error("unknown option: "+a);
    }
    require(opt.threads>0,"threads must be positive");
    require(opt.inspect || !opt.output.empty(),"--output is required for conversion");
    require(opt.input.empty()!=opt.manifest.empty(),"specify exactly one of --input and --manifest");
    OIIO::attribute("threads",opt.threads);
    OIIO::attribute("exr_threads",opt.threads);
    json items;
    if(!opt.manifest.empty()) {
        std::ifstream file(fs::u8path(opt.manifest));require(bool(file),"cannot read manifest");file>>items;
    } else items=json::array({{{"id","0"},{"path",opt.input}}});
    bool failed=false;
    for(const auto& item:items) {
        auto result=process(item,opt);
        failed=failed || !result.value("ok",false);
        std::cout<<result.dump()<<std::endl;
    }
    return failed ? 2:0;
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);
        s.pop_back();args.push_back(std::move(s));
    }
#else
int main(int argc,char** argv) {
    std::vector<std::string> args(argv,argv+argc);
#endif
    try{return run(args);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
