// Archived native candidate; not a product dependency.
#include <vips/vips.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace fs=std::filesystem;
using json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Unref { void operator()(VipsImage* p) const {if(p)g_object_unref(p);} };
using Image=std::unique_ptr<VipsImage,Unref>;
static void check(int code) {
    if(code) {std::string message=vips_error_buffer();vips_error_clear();throw std::runtime_error(message);}
}
static double ms(Clock::time_point t) {
    return std::chrono::duration<double,std::milli>(Clock::now()-t).count();
}
static int run(const std::vector<std::string>& args) {
    check(VIPS_INIT(args[0].c_str()));
    std::string manifest,output;
    int threads=4;
    for(size_t i=1;i<args.size();++i) {
        auto value=[&](){if(i+1>=args.size())throw std::runtime_error("missing option value");return args[++i];};
        const auto& a=args[i];
        if(a=="--version") {std::cout<<json({{"tool","thumbgen-vips-probe"},{"vips",vips_version_string()}}).dump()<<'\n';vips_shutdown();return 0;}
        if(a=="--manifest")manifest=value();
        else if(a=="--output")output=value();
        else if(a=="--threads")threads=std::stoi(value());
        else throw std::runtime_error("unknown option: "+a);
    }
    if(manifest.empty()||output.empty()||threads<1)throw std::runtime_error("manifest, output, positive threads required");
    vips_concurrency_set(threads);
    vips_cache_set_max(0);
    std::ifstream file(fs::u8path(manifest));
    json items;file>>items;
    fs::create_directories(fs::u8path(output));
    bool failed=false;
    for(const auto& item:items) {
        auto start=Clock::now();
        json r{{"id",item.at("id")},{"path",item.at("path")},{"ok",false}};
        try {
            auto id=item.at("id").get<std::string>();
            if(id.empty()||id.find_first_not_of("0123456789abcdef")!=std::string::npos)
                throw std::runtime_error("id must be hexadecimal");
            VipsImage* ptr=nullptr;
            auto tick=Clock::now();
            check(vips_thumbnail(item.at("path").get<std::string>().c_str(),&ptr,960,
                                 "height",960,"size",VIPS_SIZE_DOWN,nullptr));
            Image lazy(ptr);
            Image image(vips_image_copy_memory(lazy.get()));
            if(!image)check(-1);
            r["thumbnail_ms"]=ms(tick);
            r["outputs"]=json::array();
            for(auto box:{std::pair<int,int>{960,960},{425,260}}) {
                tick=Clock::now();
                double factor=std::min({double(box.first)/image->Xsize,double(box.second)/image->Ysize,1.0});
                Image resized;
                VipsImage* target=image.get();
                if(factor<1.0) {
                    ptr=nullptr;check(vips_resize(image.get(),&ptr,factor,nullptr));resized.reset(ptr);target=resized.get();
                }
                fs::path dest=fs::u8path(output)/(id+"-"+std::to_string(box.first)+".jpg");
                if(fs::exists(dest))throw std::runtime_error("output exists; use fresh output directory");
                fs::path temp=dest;temp+=".partial.jpg";
                check(vips_jpegsave(target,temp.u8string().c_str(),"Q",90,"strip",TRUE,nullptr));
                fs::rename(temp,dest);
                r["outputs"].push_back({{"path",dest.u8string()},{"width",target->Xsize},
                    {"height",target->Ysize},{"resize_and_write_ms",ms(tick)}});
            }
            r["ok"]=true;
        } catch(const std::exception& e) {r["error"]=e.what();failed=true;}
        r["total_ms"]=ms(start);
        std::cout<<r.dump()<<std::endl;
    }
    vips_shutdown();
    return failed?2:0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string s(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);
        s.pop_back();args.push_back(std::move(s));
    }
#else
int main(int argc,char** argv) {
    std::vector<std::string> args(argv,argv+argc);
#endif
    try{return run(args);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
