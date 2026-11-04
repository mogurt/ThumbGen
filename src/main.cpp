#include <thumbgen/core.h>
#include <algorithm>
#include <cctype>
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
#endif

namespace fs=std::filesystem;
using thumbgen::Json;
using thumbgen::Options;
using thumbgen::Job;
using thumbgen::Output;

static void usage() {
    std::cout << "ThumbGen 0.1.0-dev\n"
        "  thumbgen convert INPUT --output-dir DIR [--size 1024] [--json]\n"
        "  thumbgen convert INPUT --output FILE [--json]\n"
        "  thumbgen batch --manifest FILE [--json]\n"
        "  thumbgen inspect INPUT [--json]\n"
        "  thumbgen capabilities [--json]\n"
        "Options: --preset fast|balanced|quality, --format jpeg|png|webp, --quality 1..100\n"
        "  --size N or WxH (repeatable), --background black|white|transparent\n"
        "  --input-space SPACE, --ocio-config FILE, --display NAME --view NAME\n"
        "  --channels RGB|GROUP, --part N, --threads N, --max-decode-mib N\n"
        "Defaults: balanced, JPEG Q90, fit 1024x1024 without upscaling, black background.\n"
        "No output overwriting. Exit: 0 success, 1 internal, 2 usage, 3 partial, 4 all failed.\n";
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
    if(o.part < -1)throw std::invalid_argument("part must be nonnegative");
    if(o.input_space.empty())throw std::invalid_argument("input_space cannot be empty");
    if(o.display.empty()!=o.view.empty())throw std::invalid_argument("display and view must be specified together");
}
static Options options(const Json& values,bool partial=false) {
    keys(values,{"preset","input_space","background","ocio_config","display","view","channels",
        "part","threads","max_decode_mib","quality","format","size"});
    for(const char* key:{"part","threads","max_decode_mib","quality"})
        if(values.contains(key)&&!values[key].is_number_integer())throw std::invalid_argument(std::string(key)+" must be an integer");
    for(auto bound:{std::make_pair("part",2147483647),std::make_pair("threads",256),
                    std::make_pair("max_decode_mib",16384),std::make_pair("quality",100)}) {
        if(!values.contains(bound.first))continue;
        auto n=values[bound.first].get<long double>();
        auto minimum=std::string(bound.first)=="part"?-1:1;
        if(n<minimum||n>bound.second)throw std::invalid_argument(std::string(bound.first)+" out of range");
    }
    Options o;
    o.preset=values.value("preset",o.preset);o.input_space=values.value("input_space",o.input_space);
    o.background=values.value("background",o.background);o.ocio_config=values.value("ocio_config",o.ocio_config);
    o.display=values.value("display",o.display);o.view=values.value("view",o.view);o.channels=values.value("channels",o.channels);
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
    keys(object,{"path","format","quality","size"});
    if(object.contains("quality")&&!object["quality"].is_number_integer())throw std::invalid_argument("quality must be an integer");
    if(object.contains("quality") && (object["quality"].get<long double>()<1 || object["quality"].get<long double>()>100))
        throw std::invalid_argument("quality must be in 1..100");
    Output out;
    out.path=path_string(base,object.at("path").get<std::string>());
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
    for(const auto& job:jobs)inputs.insert(identity(fs::u8path(job.input)));
    for(const auto& job:jobs) {
        if(job.id.empty()||!ids.insert(job.id).second)throw std::invalid_argument("empty or duplicate job id");
        if(job.outputs.empty())throw std::invalid_argument("each job needs at least one output");
        for(const auto& out:job.outputs) {
            auto key=identity(fs::u8path(out.path));
            if(inputs.count(key))throw std::invalid_argument("output aliases an input path");
            if(!outputs.insert(key).second)throw std::invalid_argument("duplicate output path");
        }
    }
}
static std::vector<Job> manifest(const std::string& filename) {
    fs::path path=fs::absolute(fs::u8path(filename));
    std::ifstream file(path);
    if(!file)throw std::invalid_argument("cannot open manifest");
    Json root;file>>root;
    keys(root,{"schema_version","defaults","jobs"});
    if(!root.contains("schema_version")||!root["schema_version"].is_number_integer()||root["schema_version"]!=1)
        throw std::invalid_argument("unsupported schema_version; expected integer 1");
    auto defaults=root.value("defaults",Json::object());options(defaults,true);
    if(!root.at("jobs").is_array()||root["jobs"].empty())throw std::invalid_argument("jobs must be a nonempty array");
    std::vector<Job> jobs;
    for(const auto& object:root["jobs"]) {
        keys(object,{"id","input","options","outputs"});
        Job job;job.id=object.at("id").get<std::string>();
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
            {"error",{{"code",code},{"message",e.what()}}}}).dump()<<std::endl;
        else std::cerr<<"thumbgen: "<<e.what()<<std::endl;
        return exit_code;
    };
    try {
        if(args.size()<2||std::find(args.begin(),args.end(),"--help")!=args.end()){usage();return 0;}
        thumbgen::Engine engine;
        auto command=args[1];
        if(command=="capabilities"||command=="--version") {
            for(size_t i=2;i<args.size();++i)if(args[i]!="--json")throw std::invalid_argument("unexpected argument");
            auto data=thumbgen::Engine::capabilities();
            if(command=="--version")data={{"schema_version",1},{"type","version"},{"version",data["version"]},{"oiio_version",data["oiio_version"]}};
            std::cout<<data.dump(json_mode?-1:2)<<std::endl;return 0;
        }
        if(command!="convert"&&command!="batch"&&command!="inspect")throw std::invalid_argument("unknown command: "+command);
        Json values=Json::object();std::vector<std::pair<int,int>> sizes;
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
            else if(flag=="--part")values["part"]=number(value());
            else if(flag=="--max-decode-mib")values["max_decode_mib"]=number(value());
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
            if(input.empty()||!values.empty()||!sizes.empty()||!output_dir.empty()||!output_file.empty()||!manifest_file.empty())
                throw std::invalid_argument("inspect expects only INPUT and optional --json");
            auto r=engine.inspect(path_string(fs::current_path(),input));
            std::cout<<r.dump(json_mode?-1:2)<<std::endl;return r["ok"].get<bool>()?0:4;
        }
        std::vector<Job> jobs;
        if(command=="batch") {
            if(manifest_file.empty()||!input.empty()||!output_file.empty()||!output_dir.empty()||!sizes.empty()||!values.empty())
                throw std::invalid_argument("batch expects --manifest FILE and optional --json; settings belong in the manifest");
            jobs=manifest(manifest_file);
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
        size_t success=0,partial=0,failed=0;
        for(const auto& job:jobs) {
            auto r=engine.convert(job);
            if(r["status"]=="success")++success;
            else if(r["status"]=="partial")++partial;
            else ++failed;
            if(json_mode)std::cout<<r.dump()<<std::endl;
            else std::cout<<job.input<<": "<<r["status"].get<std::string>()<<'\n';
            if(!r["ok"].get<bool>()&&!json_mode)std::cerr<<r.dump(2)<<'\n';
        }
        Json end{{"schema_version",1},{"type","summary"},{"total",jobs.size()},
                 {"success",success},{"partial",partial},{"failed",failed}};
        if(json_mode)std::cout<<end.dump()<<std::endl;
        if(success==jobs.size())return 0;
        return success||partial?3:4;
    }catch(const std::invalid_argument& e){return error("invalid_request",e,2);}
     catch(const std::out_of_range& e){return error("invalid_request",e,2);}
     catch(const Json::exception& e){return error("invalid_request",e,2);}
     catch(const fs::filesystem_error& e){return error("invalid_request",e,2);}
     catch(const std::exception& e){return error("internal_error",e,1);}
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
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
