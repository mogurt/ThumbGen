#include <thumbgen/core.h>
#include "files.h"
#include "isolated.h"
#include "data_preview.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs=std::filesystem;
using thumbgen::Json;
using thumbgen::Options;
using thumbgen::Job;
using thumbgen::Output;

namespace {
std::atomic<bool> interrupt_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,"signal flag must be lock free");
#ifdef _WIN32
BOOL WINAPI interrupt_handler(DWORD kind) {
    if(kind!=CTRL_C_EVENT && kind!=CTRL_BREAK_EVENT)return FALSE;
    interrupt_requested.store(true,std::memory_order_relaxed);return TRUE;
}
#else
void interrupt_handler(int){interrupt_requested.store(true,std::memory_order_relaxed);}
#endif
class InterruptGuard {
#ifndef _WIN32
    struct sigaction old_int{},old_term{};
#endif
public:
    InterruptGuard() {
        interrupt_requested.store(false,std::memory_order_relaxed);
#ifdef _WIN32
        if(!SetConsoleCtrlHandler(interrupt_handler,TRUE))throw std::system_error(int(GetLastError()),std::system_category(),"install interrupt handler");
#else
        struct sigaction action{};action.sa_handler=interrupt_handler;sigemptyset(&action.sa_mask);action.sa_flags=SA_RESTART;
        if(sigaction(SIGINT,&action,&old_int)!=0)throw std::system_error(errno,std::generic_category(),"install SIGINT handler");
        if(sigaction(SIGTERM,&action,&old_term)!=0){auto code=errno;sigaction(SIGINT,&old_int,nullptr);throw std::system_error(code,std::generic_category(),"install SIGTERM handler");}
#endif
    }
    ~InterruptGuard() {
#ifdef _WIN32
        SetConsoleCtrlHandler(interrupt_handler,FALSE);
#else
        sigaction(SIGTERM,&old_term,nullptr);sigaction(SIGINT,&old_int,nullptr);
#endif
    }
    static bool requested(){return interrupt_requested.load(std::memory_order_relaxed);}
};
}

static void usage() {
    std::cout << "ThumbGen 0.1.0\n"
        "  thumbgen convert INPUT --output-dir DIR [--size 1024] [--json]\n"
        "  thumbgen convert INPUT --output FILE [--json]\n"
        "  thumbgen batch --manifest FILE [--json]\n"
        "  thumbgen inspect INPUT [--json]\n"
        "  thumbgen capabilities [--json]\n"
        "Options: --preset fast|balanced|quality, --format jpeg|png|webp, --quality 1..100\n"
        "  --size N or WxH (repeatable), --background black|white|transparent\n"
        "  --input-space SPACE, --ocio-config FILE, --display NAME --view NAME\n"
        "  --channels RGB|GRAY|GROUP, --part N or --part-name NAME\n"
        "  --selection-policy primary|root_rgb, --threads N, --max-decode-mib N\n"
        "  --deep-mode reject|flatten, --deep-samples-per-pixel 1..1048576\n"
        "  --visualize colour|depth|normal|vector|id|id_rgb|cryptomatte, --data-range MIN,MAX, --invalid-values black|magenta|error\n"
        "  --existing error|skip|replace (default: error)\n"
        "  --workers N, --thread-budget N, --memory-budget-mib N, --startup-timeout-ms N, --task-timeout-ms N\n"
        "Defaults: balanced, JPEG Q90, fit 1024x1024 without upscaling, black background.\n"
        "Default: no output overwriting. Exit: 0 success, 1 internal, 2 usage, 3 partial, 4 all failed, 130 interrupted.\n";
}
static void keys(const Json& object,const std::set<std::string>& allowed) {
    if(!object.is_object())throw std::invalid_argument("expected an object");
    for(auto it=object.begin();it!=object.end();++it)
        if(!allowed.count(it.key()))throw std::invalid_argument("unknown field: "+it.key());
}
static std::pair<int,int> size_value(const Json& v) {
    auto dimension=[](const Json& value) {
        if(!value.is_number_integer() || value.get<long double>()<1 || value.get<long double>()>32768)
            throw std::invalid_argument("size must be an integer in 1..32768");
        return value.get<int>();
    };
    int w=0,h=0;
    if(v.is_number_integer())w=h=dimension(v);
    else if(v.is_array() && v.size()==2 && v[0].is_number_integer() && v[1].is_number_integer()) {
        w=dimension(v[0]);h=dimension(v[1]);
    }else throw std::invalid_argument("size must be an integer or [width,height]");
    if(w<1||h<1||w>32768||h>32768)throw std::invalid_argument("size must be in 1..32768");
    return {w,h};
}
static int number(const std::string& text) {
    size_t used=0;
    int n=std::stoi(text,&used);
    if(used!=text.size())throw std::invalid_argument("invalid integer: "+text);
    return n;
}
static std::pair<int,int> parse_size(const std::string& text) {
    auto x=text.find('x');
    int w=number(text.substr(0,x)),h=x==std::string::npos?w:number(text.substr(x+1));
    return size_value(Json::array({w,h}));
}
static void validate_options(const Options& o) {
    if(o.preset!="fast"&&o.preset!="balanced"&&o.preset!="quality")throw std::invalid_argument("invalid preset");
    if(o.background!="black"&&o.background!="white"&&o.background!="transparent")throw std::invalid_argument("invalid background");
    if(o.threads<1||o.threads>256)throw std::invalid_argument("threads must be in 1..256");
    if(o.max_decode_mib<1||o.max_decode_mib>16384)throw std::invalid_argument("max_decode_mib must be in 1..16384");
    if(o.deep_mode!="reject" && o.deep_mode!="flatten")throw std::invalid_argument("invalid deep_mode");
    if(o.deep_samples_per_pixel<1 || o.deep_samples_per_pixel>1048576)throw std::invalid_argument("deep_samples_per_pixel must be in 1..1048576");
    if(o.deep_mode=="flatten" && o.visualization!="colour")throw std::invalid_argument("Deep flattening requires colour visualization");
    if(o.part < -1)throw std::invalid_argument("part must be nonnegative");
    if(o.part>=0 && !o.part_name.empty())throw std::invalid_argument("part and part_name are mutually exclusive");
    if(o.selection_policy!="primary" && o.selection_policy!="root_rgb")throw std::invalid_argument("invalid selection_policy");
    if(o.input_space.empty())throw std::invalid_argument("input_space cannot be empty");
    if(o.display.empty()!=o.view.empty())throw std::invalid_argument("display and view must be specified together");
    auto mapping=thumbgen::detail::data_mapping(o.visualization,o.data_range,o.invalid_values);
    if(mapping.mode!=thumbgen::detail::DataMode::colour) {
        if(o.channels.empty())throw std::invalid_argument("data visualization requires explicit channels");
        if(o.input_space!="auto" || !o.ocio_config.empty() || !o.display.empty())throw std::invalid_argument("data visualization does not accept colour transform options");
    }
}
static Options options(const Json& values,bool partial=false) {
    keys(values,{"preset","input_space","background","ocio_config","display","view","channels","part_name","selection_policy",
        "part","threads","max_decode_mib","quality","format","size","existing","visualization","data_range","invalid_values","deep_mode","deep_samples_per_pixel"});
    if(values.contains("existing")) {
        auto policy=values["existing"].get<std::string>();
        if(policy!="error" && policy!="skip" && policy!="replace")throw std::invalid_argument("invalid existing policy");
    }
    for(const char* key:{"part","threads","max_decode_mib","quality","deep_samples_per_pixel"})
        if(values.contains(key)&&!values[key].is_number_integer())throw std::invalid_argument(std::string(key)+" must be an integer");
    for(auto bound:{std::make_pair("part",2147483647),std::make_pair("threads",256),
                    std::make_pair("max_decode_mib",16384),std::make_pair("quality",100),std::make_pair("deep_samples_per_pixel",1048576)}) {
        if(!values.contains(bound.first))continue;
        auto n=values[bound.first].get<long double>();
        auto minimum=std::string(bound.first)=="part"?-1:1;
        if(n<minimum||n>bound.second)throw std::invalid_argument(std::string(bound.first)+" out of range");
    }
    Options o;
    o.deep_mode=values.value("deep_mode",o.deep_mode);o.deep_samples_per_pixel=values.value("deep_samples_per_pixel",o.deep_samples_per_pixel);
    o.visualization=values.value("visualization",o.visualization);o.invalid_values=values.value("invalid_values",o.invalid_values);
    if(values.contains("data_range")) {
        const auto& range=values["data_range"];
        if(!range.is_array() || (!range.empty() && (range.size()!=2 || !range[0].is_number() || !range[1].is_number())))throw std::invalid_argument("data_range must be empty or contain two numbers");
        o.data_range=range.get<std::vector<double>>();
    }
    o.preset=values.value("preset",o.preset);o.input_space=values.value("input_space",o.input_space);
    o.background=values.value("background",o.background);o.ocio_config=values.value("ocio_config",o.ocio_config);
    o.display=values.value("display",o.display);o.view=values.value("view",o.view);o.channels=values.value("channels",o.channels);
    o.part_name=values.value("part_name",o.part_name);o.selection_policy=values.value("selection_policy",o.selection_policy);
    if(values.contains("part_name") && o.part_name.empty())throw std::invalid_argument("part_name cannot be empty");
    o.part=values.value("part",o.part);o.threads=values.value("threads",o.threads);
    if(values.contains("max_decode_mib")) {
        auto value=values.at("max_decode_mib").get<long long>();
        if(value<1||value>16384)throw std::invalid_argument("max_decode_mib must be in 1..16384");
        o.max_decode_mib=size_t(value);
    }
    if(!partial)validate_options(o);
    return o;
}
static std::string path_string(const fs::path& base,const std::string& text) {
    if(text.empty()||text.find('\0')!=std::string::npos)throw std::invalid_argument("path cannot be empty or contain NUL");
    return fs::absolute(base/fs::u8path(text)).lexically_normal().u8string();
}
static std::string infer_format(const std::string& path) {
    auto ext=fs::u8path(path).extension().u8string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(ext==".jpg"||ext==".jpeg")return "jpeg";
    if(ext==".png")return "png";
    if(ext==".webp")return "webp";
    throw std::invalid_argument("output extension must be jpg, jpeg, png or webp");
}
static Output output(const Json& object,const Json& defaults,const fs::path& base,const Options& o) {
    keys(object,{"path","format","quality","size","existing"});
    if(object.contains("quality")&&!object["quality"].is_number_integer())throw std::invalid_argument("quality must be an integer");
    if(object.contains("quality") && (object["quality"].get<long double>()<1 || object["quality"].get<long double>()>100))
        throw std::invalid_argument("quality must be in 1..100");
    Output out;
    out.path=path_string(base,object.at("path").get<std::string>());
    out.existing=object.value("existing",defaults.value("existing",std::string("error")));
    if(out.existing!="error" && out.existing!="skip" && out.existing!="replace")throw std::invalid_argument("invalid existing policy");
    out.format=object.value("format",defaults.value("format",infer_format(out.path)));
    if(out.format=="jpg")out.format="jpeg";
    if(out.format!=infer_format(out.path))throw std::invalid_argument("format disagrees with output extension");
    out.quality=object.value("quality",defaults.value("quality",90));
    if(out.quality<1||out.quality>100)throw std::invalid_argument("quality must be in 1..100");
    auto size=size_value(object.value("size",defaults.value("size",Json::array({1024,1024}))));
    out.width=size.first;out.height=size.second;
    if(out.format=="jpeg" && o.background=="transparent")throw std::invalid_argument("JPEG cannot preserve transparency; select a background or PNG/WebP");
    return out;
}
static std::string identity(const fs::path& path) {
    return fs::weakly_canonical(path).u8string();
}
struct PathLess {
    bool operator()(const std::string& a,const std::string& b) const {
#ifdef _WIN32
        auto x=fs::u8path(a).native(),y=fs::u8path(b).native();
        return CompareStringOrdinal(x.data(),int(x.size()),y.data(),int(y.size()),TRUE)==CSTR_LESS_THAN;
#else
        return a<b;
#endif
    }
};
static void preflight(const std::vector<Job>& jobs) {
    std::set<std::string> ids;
    std::set<std::string,PathLess> outputs,inputs;
    std::set<std::string> input_files,output_files;
    for(const auto& job:jobs) {
        inputs.insert(identity(fs::u8path(job.input)));
        auto key=thumbgen::detail::file_identity(fs::u8path(job.input));if(!key.empty())input_files.insert(key);
    }
    for(const auto& job:jobs) {
        if(job.id.empty()||!ids.insert(job.id).second)throw std::invalid_argument("empty or duplicate job id");
        if(job.outputs.empty())throw std::invalid_argument("each job needs at least one output");
        for(const auto& out:job.outputs) {
            auto key=identity(fs::u8path(out.path));
            if(inputs.count(key))throw std::invalid_argument("output aliases an input path");
            if(!outputs.insert(key).second)throw std::invalid_argument("duplicate output path");
            auto file=thumbgen::detail::file_identity(fs::u8path(out.path));
            if(!file.empty()) {
                if(input_files.count(file))throw std::invalid_argument("output hard-links or aliases an input file");
                if(!output_files.insert(file).second)throw std::invalid_argument("outputs refer to the same existing file");
            }
        }
    }
}
static thumbgen::detail::IsolationOptions execution_options(const Json& value) {
    keys(value,{"workers","thread_budget","memory_budget_mib","startup_timeout_ms","task_timeout_ms"});
    auto number=[&](const char* key,int minimum,int maximum,int fallback) {
        if(!value.contains(key))return fallback;
        const auto& item=value[key];
        if(!item.is_number_integer() || item.get<long double>()<minimum || item.get<long double>()>maximum)
            throw std::invalid_argument(std::string("invalid execution field: ")+key);
        return item.get<int>();
    };
    thumbgen::detail::IsolationOptions result;
    result.workers=unsigned(number("workers",1,256,1));
    result.thread_budget=unsigned(number("thread_budget",1,256,0));
    result.memory_mib=size_t(number("memory_budget_mib",0,2147483647,0));
    result.startup_timeout=std::chrono::milliseconds(number("startup_timeout_ms",1,86400000,30000));
    result.task_timeout=std::chrono::milliseconds(number("task_timeout_ms",0,86400000,0));return result;
}
static std::vector<Job> manifest(const std::string& filename,thumbgen::detail::IsolationOptions& execution) {
    if(filename=="-")throw std::invalid_argument("stdin manifests are not supported; pass a manifest file");
    fs::path path=fs::absolute(fs::u8path(filename));
    std::ifstream file(path);
    if(!file)throw std::invalid_argument("cannot open manifest");
    Json root;file>>root;
    keys(root,{"schema_version","defaults","jobs","execution"});
    if(!root.contains("schema_version")||!root["schema_version"].is_number_integer()||root["schema_version"]!=1)
        throw std::invalid_argument("unsupported schema_version; expected integer 1");
    auto defaults=root.value("defaults",Json::object());options(defaults,true);
    execution=execution_options(root.value("execution",Json::object()));
    if(!root.at("jobs").is_array()||root["jobs"].empty())throw std::invalid_argument("jobs must be a nonempty array");
    std::vector<Job> jobs;
    for(const auto& object:root["jobs"]) {
        keys(object,{"id","input","sequence_id","options","outputs"});
        Job job;job.id=object.at("id").get<std::string>();
        if(object.contains("sequence_id")) {
            job.sequence_id=object.at("sequence_id").get<std::string>();
            if(job.sequence_id.empty())throw std::invalid_argument("sequence_id cannot be empty");
        }
        job.input=path_string(path.parent_path(),object.at("input").get<std::string>());
        auto override=object.value("options",Json::object());options(override,true);
        auto effective=defaults;effective.update(override);job.options=options(effective);
        if(!job.options.ocio_config.empty() && job.options.ocio_config.rfind("ocio://",0)!=0)
            job.options.ocio_config=path_string(path.parent_path(),job.options.ocio_config);
        if(!object.at("outputs").is_array())throw std::invalid_argument("outputs must be an array");
        for(const auto& value:object["outputs"])job.outputs.push_back(output(value,effective,path.parent_path(),job.options));
        jobs.push_back(std::move(job));
    }
    preflight(jobs);return jobs;
}

static int execute(const std::vector<std::string>& args) {
    bool json_mode=std::find(args.begin(),args.end(),"--json")!=args.end();
    auto error=[&](const char* code,const std::exception& e,int exit_code) {
        if(json_mode)std::cout<<Json({{"schema_version",1},{"type","error"},
            {"error",{{"code",code},{"message",e.what()}}}}).dump(-1,' ',false,Json::error_handler_t::replace)<<std::endl;
        else std::cerr<<"thumbgen: "<<e.what()<<std::endl;
        return exit_code;
    };
    try {
        if(args.size()>1 && args[1]=="--internal-worker") {
            if(args.size()!=5)throw std::invalid_argument("invalid internal worker invocation");
            return thumbgen::detail::serve_worker(args[2],args[3],unsigned(number(args[4])));
        }
        if(args.size()<2||std::find(args.begin(),args.end(),"--help")!=args.end()){usage();return 0;}
        thumbgen::Engine engine;
        auto command=args[1];
        if(command=="capabilities"||command=="--version") {
            for(size_t i=2;i<args.size();++i)if(args[i]!="--json")throw std::invalid_argument("unexpected argument");
            auto data=thumbgen::Engine::capabilities();
            if(command=="--version")data={{"schema_version",1},{"type","version"},{"version",data["version"]},{"oiio_version",data["oiio_version"]},{"build",data["build"]}};
            std::cout<<data.dump(json_mode?-1:2,' ',false,Json::error_handler_t::replace)<<std::endl;return 0;
        }
        if(command!="convert"&&command!="batch"&&command!="inspect")throw std::invalid_argument("unknown command: "+command);
        Json values=Json::object(),execution_values=Json::object();std::vector<std::pair<int,int>> sizes;
        std::string input,output_file,output_dir,manifest_file;
        for(size_t i=2;i<args.size();++i) {
            const auto flag=args[i];
            auto value=[&](){if(i+1>=args.size())throw std::invalid_argument("missing value: "+flag);return args[++i];};
            if(flag=="--json")continue;
            if(flag=="--manifest")manifest_file=value();
            else if(flag=="--output")output_file=value();
            else if(flag=="--output-dir")output_dir=value();
            else if(flag=="--size")sizes.push_back(parse_size(value()));
            else if(flag=="--quality")values["quality"]=number(value());
            else if(flag=="--threads")values["threads"]=number(value());
            else if(flag=="--workers")execution_values["workers"]=number(value());
            else if(flag=="--thread-budget")execution_values["thread_budget"]=number(value());
            else if(flag=="--memory-budget-mib")execution_values["memory_budget_mib"]=number(value());
            else if(flag=="--startup-timeout-ms")execution_values["startup_timeout_ms"]=number(value());
            else if(flag=="--task-timeout-ms")execution_values["task_timeout_ms"]=number(value());
            else if(flag=="--part")values["part"]=number(value());
            else if(flag=="--part-name")values["part_name"]=value();
            else if(flag=="--selection-policy")values["selection_policy"]=value();
            else if(flag=="--visualize")values["visualization"]=value();
            else if(flag=="--invalid-values")values["invalid_values"]=value();
            else if(flag=="--data-range") {
                auto text=value();auto comma=text.find(',');
                if(comma==std::string::npos || text.find(',',comma+1)!=std::string::npos)throw std::invalid_argument("data range must be MIN,MAX");
                auto numeric=[](const std::string& text){size_t used=0;auto n=std::stod(text,&used);if(used!=text.size())throw std::invalid_argument("invalid range number");return n;};
                values["data_range"]=Json::array({numeric(text.substr(0,comma)),numeric(text.substr(comma+1))});
            }
            else if(flag=="--existing")values["existing"]=value();
            else if(flag=="--max-decode-mib")values["max_decode_mib"]=number(value());
            else if(flag=="--deep-mode")values["deep_mode"]=value();
            else if(flag=="--deep-samples-per-pixel")values["deep_samples_per_pixel"]=number(value());
            else if(flag=="--preset")values["preset"]=value();
            else if(flag=="--format")values["format"]=value();
            else if(flag=="--background")values["background"]=value();
            else if(flag=="--input-space")values["input_space"]=value();
            else if(flag=="--ocio-config")values["ocio_config"]=value();
            else if(flag=="--display")values["display"]=value();
            else if(flag=="--view")values["view"]=value();
            else if(flag=="--channels")values["channels"]=value();
            else if(flag.rfind("--",0)==0)throw std::invalid_argument("unknown option: "+flag);
            else if(input.empty())input=flag;
            else throw std::invalid_argument("unexpected argument: "+flag);
        }
        if(command=="inspect") {
            if(input.empty()||!values.empty()||!execution_values.empty()||!sizes.empty()||!output_dir.empty()||!output_file.empty()||!manifest_file.empty())
                throw std::invalid_argument("inspect expects only INPUT and optional --json");
            auto r=engine.inspect(path_string(fs::current_path(),input));
            std::cout<<r.dump(json_mode?-1:2,' ',false,Json::error_handler_t::replace)<<std::endl;return r["ok"].get<bool>()?0:4;
        }
        std::vector<Job> jobs;
        auto execution=execution_options(execution_values);
        if(command=="batch") {
            if(manifest_file.empty()||!input.empty()||!output_file.empty()||!output_dir.empty()||!sizes.empty()||!values.empty()||!execution_values.empty())
                throw std::invalid_argument("batch expects --manifest FILE and optional --json; settings belong in the manifest");
            jobs=manifest(manifest_file,execution);
        } else {
            if(input.empty()||!manifest_file.empty()||output_dir.empty()==output_file.empty())
                throw std::invalid_argument("convert requires INPUT and exactly one of --output/--output-dir");
            Job job;job.id="0";job.input=path_string(fs::current_path(),input);job.options=options(values);
            if(sizes.empty())sizes.push_back({1024,1024});
            if(!output_file.empty()&&sizes.size()!=1)throw std::invalid_argument("--output accepts only one size");
            std::string format=values.value("format",std::string("jpeg"));if(format=="jpg")format="jpeg";
            for(auto size:sizes) {
                std::string path=output_file;
                if(path.empty())path=(fs::u8path(output_dir)/fs::u8path(fs::u8path(input).filename().u8string()+"."+
                    std::to_string(size.first)+"x"+std::to_string(size.second)+"."+(format=="jpeg"?"jpg":format))).u8string();
                job.outputs.push_back(output({{"path",path},{"size",{size.first,size.second}}},values,fs::current_path(),job.options));
            }
            jobs.push_back(std::move(job));preflight(jobs);
        }
        const auto batch_start=std::chrono::steady_clock::now();
        size_t success=0,partial=0,failed=0,skipped=0,cancelled=0,outputs_total=0,outputs_success=0,outputs_failed=0,outputs_skipped=0,outputs_cancelled=0;
        for(const auto& job:jobs)outputs_total+=job.outputs.size();
        std::string executable;
#ifdef _WIN32
        std::vector<wchar_t> path(1024);
        for(;;) {
            auto count=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
            if(!count)throw std::runtime_error("cannot locate the running executable");
            if(count<path.size()){executable=fs::path(std::wstring(path.data(),count)).u8string();break;}
            if(path.size()>=32768)throw std::runtime_error("executable path exceeds Windows limit");
            path.resize(std::min<size_t>(32768,path.size()*2));
        }
#else
        executable=fs::read_symlink("/proc/self/exe").u8string();
#endif
        const auto execution_settings=thumbgen::detail::execution_configuration(jobs,execution);
        InterruptGuard interrupt_guard;execution.cancelled=InterruptGuard::requested;
        const bool interrupted=thumbgen::detail::run_isolated(jobs,executable,[&](const Json& r) {
            if(r["status"]=="success")++success;
            else if(r["status"]=="partial")++partial;
            else if(r["status"]=="skipped")++skipped;
            else if(r["status"]=="cancelled")++cancelled;
            else ++failed;
            for(const auto& out:r["outputs"]) {
                if(out["status"]=="success")++outputs_success;
                else if(out["status"]=="skipped")++outputs_skipped;
                else if(out["status"]=="cancelled")++outputs_cancelled;
                else ++outputs_failed;
            }
            if(json_mode)std::cout<<r.dump(-1,' ',false,Json::error_handler_t::replace)<<std::endl;
            else std::cout<<r["input"].get<std::string>()<<": "<<r["status"].get<std::string>()<<'\n';
            if(!r["ok"].get<bool>()&&!json_mode)std::cerr<<r.dump(2)<<'\n';
        },execution);
        Json end{{"schema_version",1},{"type","summary"},{"total",jobs.size()},
                 {"success",success},{"partial",partial},{"failed",failed},{"skipped",skipped},{"cancelled",cancelled},{"interrupted",interrupted},
                 {"output_counts",{{"total",outputs_total},{"success",outputs_success},{"failed",outputs_failed},
                     {"skipped",outputs_skipped},{"cancelled",outputs_cancelled},{"not_attempted",outputs_total-outputs_success-outputs_failed-outputs_skipped-outputs_cancelled}}},
                 {"elapsed_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-batch_start).count()},
                 {"execution",execution_settings}};
        if(json_mode)std::cout<<end.dump()<<std::endl;
        if(interrupted)return 130;
        if(success+skipped==jobs.size())return 0;
        return success||partial||skipped?3:4;
    }catch(const std::invalid_argument& e){return error("invalid_request",e,2);}
     catch(const std::out_of_range& e){return error("invalid_request",e,2);}
     catch(const Json::exception& e){return error("invalid_request",e,2);}
     catch(const fs::filesystem_error& e){return error("invalid_request",e,2);}
     catch(const std::exception& e){return error("internal_error",e,1);}
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string value(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),n,nullptr,nullptr);
        value.pop_back();args.push_back(std::move(value));
    }
#else
int main(int argc,char** argv) {
    std::vector<std::string> args(argv,argv+argc);
#endif
    return execute(args);
}
