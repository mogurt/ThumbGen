#include "png_scanline.h"
#include "cms_input.h"
#include <iostream>
#include <chrono>
#include <stdexcept>
using namespace thumbgen::detail;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct TrackedScratch {
    inline static std::size_t allocations=0,samples=0;
    std::vector<std::uint16_t> storage;
    bool empty() const {return storage.empty();}
    void resize(std::size_t n){++allocations;samples=n;storage.resize(n);}
    std::uint16_t* data(){return storage.data();}
    static void reset(){allocations=0;samples=0;}
};
static void numeric(int w,int h,int nc,bool gamma) {
    ColourPlan plan;plan.route=ColourPlan::Route::integer_linear;plan.sample_bytes=2;plan.input=*skcms_sRGB_profile();
    if(gamma){const skcms_TransferFunction trc{2.2f,1,0,0,0,0,0};skcms_SetTransferFunction(&plan.input,&trc);}
    const auto count=std::size_t(w)*h;
    std::vector<std::uint16_t> native(count*nc);
    for(std::size_t i=0;i<native.size();++i)native[i]=std::uint16_t((i*7919)^(i>>3));
    std::vector<float> expected(count*nc),block(std::min(count,colour_chunk_pixels)*nc);
    plan.transform(native.data(),expected.data(),count,nc);
    std::size_t cursor=0,calls=0;
    auto read=[&](int begin,int end,void* rows){
        check(begin>=0 && end>begin && end<=h,"invalid row range");
        check(std::size_t(begin)*w==cursor,"non-monotonic input");
        const auto n=std::size_t(end-begin)*w;cursor+=n;
        std::memcpy(rows,native.data()+std::size_t(begin)*w*nc,n*nc*2);return true;
    };
    auto transform=[&](const void* src,std::size_t base,std::size_t n){
        check(base==calls*colour_chunk_pixels,"global chunk boundary changed");++calls;
        check(n==std::min(count-base,colour_chunk_pixels),"transform tail changed");
        plan.transform(src,block.data(),n,nc);
        check(!std::memcmp(block.data(),expected.data()+base*nc,n*nc*sizeof(float)),"float bits differ");
    };
    TrackedScratch::reset();
    check((png_scanline_blocks<decltype(read),decltype(transform),TrackedScratch>(w,h,nc,read,transform)),"read failed");
    check(cursor==count,"input incomplete");
    const auto capacity=std::min(count,colour_chunk_pixels);
    const bool needs_scratch=capacity%std::size_t(w)!=0;
    check(TrackedScratch::allocations==std::size_t(needs_scratch),"scratch was not lazy or was reallocated");
    check(!needs_scratch || TrackedScratch::samples==std::size_t(w)*nc,"scratch exceeds one row");
    int reads=0;calls=0;
    check(!png_scanline_blocks(w,2,nc,[&](int,int,void*){++reads;return false;},
        [&](const void*,std::size_t,std::size_t){++calls;}),"failed read accepted");
    check(reads==1 && calls==0,"failed input retried or transformed");
}
// Verify real 8MP bulk access without allocating full native/float test arrays.
// Failures write a prefix before returning false, as a decoder may do.
static void transport(int w,int h,int fail_call=0,int expected_transforms=-1) {
    constexpr int nc=3;
    std::size_t cursor=0,base=0;int reads=0,transforms=0;
    auto value=[](std::size_t i){return std::uint16_t((i*7919)^(i>>3));};
    auto read=[&](int begin,int end,void* output) {
        check(begin>=0 && end>begin && end<=h && std::size_t(begin)*w==cursor,"range reread or overread");
        ++reads;const auto n=std::size_t(end-begin)*w;
        auto* out=static_cast<std::uint16_t*>(output);
        const auto samples=(reads==fail_call?n/2:n)*nc;
        for(std::size_t i=0;i<samples;++i)out[i]=value(cursor*nc+i);
        if(reads==fail_call)return false;
        cursor+=n;return true;
    };
    auto transform=[&](const void* data,std::size_t offset,std::size_t n) {
        const auto count=std::size_t(w)*h;
        check(offset==base && n==std::min(count-base,colour_chunk_pixels),"transport chunk/tail mismatch");
        const auto* src=static_cast<const std::uint16_t*>(data);
        for(std::size_t i=0;i<n*nc;++i)check(src[i]==value(base*nc+i),"transport pixel order mismatch");
        base+=n;++transforms;
    };
    TrackedScratch::reset();
    const bool ok=png_scanline_blocks<decltype(read),decltype(transform),TrackedScratch>(w,h,nc,read,transform);
    check(ok==(fail_call==0),"transport accepted decoder failure");
    if(fail_call) {
        check(reads==fail_call && transforms==expected_transforms,"failure retried or partial block transformed");
    } else {
        check(cursor==std::size_t(w)*h && base==cursor,"transport input incomplete");
        if(w==4096 && h==2048) {
            check(reads==8 && transforms==8,"divisible source was not read in 1M blocks");
            check(TrackedScratch::allocations==0,"divisible source allocated scratch");
        }
    }
    check(TrackedScratch::allocations<=1 && TrackedScratch::samples<=std::size_t(w)*nc,"scratch bound exceeded");
}
static void eligibility() {
    auto path=std::filesystem::temp_directory_path()/(
        "thumbgen-png-header-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{path};
    unsigned char header[33]{137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R'};
    auto set32=[](unsigned char* p,unsigned v){for(int i=3;i>=0;--i){p[i]=static_cast<unsigned char>(v);v>>=8;}};
    set32(header+16,4001);set32(header+20,2000);header[24]=16;header[25]=6;
    auto save=[&](){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<char*>(header),sizeof(header));};
    save();OIIO::ImageSpec spec(4001,2000,4,OIIO::TypeDesc::UINT16);
    ColourPlan plan;plan.route=ColourPlan::Route::integer_linear;plan.sample_bytes=2;
    auto eligible=[&](){return png_scanline_eligible(path,spec,"png",plan);};
    check(eligible(),"eligible PNG16 rejected");
    header[28]=1;save();check(!eligible(),"Adam7 accepted");header[28]=0;save();
    check(!png_scanline_eligible(path,spec,"jpeg",plan),"extension dispatch");
    spec.attribute("Orientation",2);check(!eligible(),"orientation accepted");spec.attribute("Orientation",1);
    spec.format=OIIO::TypeDesc::UINT8;check(!eligible(),"UINT8 accepted");spec.format=OIIO::TypeDesc::UINT16;
    spec.width=3999;check(!eligible(),"small image accepted");spec.width=4001;
    plan.route=ColourPlan::Route::reference;check(!eligible(),"reference override ignored");
    plan.route=ColourPlan::Route::srgb8;check(!eligible(),"srgb8 rerouted");plan.route=ColourPlan::Route::integer_linear;
    header[0]=0;save();check(!eligible(),"unrecognized header accepted");
}
int main(int argc,char**) {try {
    if(argc>1)skcms_DisableRuntimeCPUDetection();
    eligibility();
    for(int nc:{3,4})for(bool gamma:{false,true}) {
        for(int n:{1,15,16,17,1048575,1048576,1048577})numeric(n,1,nc,gamma);
        numeric(1021,1031,nc,gamma); // row and SIMD tails cross global chunks
        numeric(1048575,3,nc,gamma);
        numeric(1048576,2,nc,gamma);
        numeric(1048577,2,nc,gamma);
        numeric(2097155,1,nc,gamma); // one source row spans more than two blocks
    }
    transport(4096,2048);
    transport(4096,769,1,0); // first bulk read fails after writing a prefix
    transport(4096,769,2,1); // later bulk read: keep only the previous full block
    transport(1021,2055,2,0); // row crossing first block fails before any transform
    transport(1048579,2,2,1); // wide second row fails, no partial tail transform
    std::cout<<"PNG block float bits, chunk boundaries and failure checks passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
