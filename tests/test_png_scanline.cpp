#include "png_scanline.h"
#include "cms_input.h"
#include <iostream>
#include <chrono>
#include <stdexcept>
using namespace thumbgen::detail;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void numeric(int w,int h,int nc,bool gamma) {
    ColourPlan plan;plan.route=ColourPlan::Route::integer_linear;plan.sample_bytes=2;plan.input=*skcms_sRGB_profile();
    if(gamma){const skcms_TransferFunction trc{2.2f,1,0,0,0,0,0};skcms_SetTransferFunction(&plan.input,&trc);}
    const auto count=std::size_t(w)*h;
    std::vector<std::uint16_t> native(count*nc);
    for(std::size_t i=0;i<native.size();++i)native[i]=std::uint16_t((i*7919)^(i>>3));
    std::vector<float> expected(count*nc),block(std::min(count,colour_chunk_pixels)*nc);
    plan.transform(native.data(),expected.data(),count,nc);
    std::size_t cursor=0,calls=0;
    check(png_scanline_blocks(w,h,nc,[&](int y,void* row){
        check(std::size_t(y)*w==cursor,"non-monotonic input");cursor+=w;
        std::memcpy(row,native.data()+std::size_t(y)*w*nc,std::size_t(w)*nc*2);return true;
    },[&](const void* src,std::size_t base,std::size_t n){
        check(base==calls*colour_chunk_pixels,"global chunk boundary changed");++calls;
        plan.transform(src,block.data(),n,nc);
        check(!std::memcmp(block.data(),expected.data()+base*nc,n*nc*sizeof(float)),"float bits differ");
    }),"read failed");
    check(cursor==count,"input incomplete");
    int reads=0;calls=0;
    check(!png_scanline_blocks(w,2,nc,[&](int,void*){++reads;return false;},
        [&](const void*,std::size_t,std::size_t){++calls;}),"failed read accepted");
    check(reads==1 && calls==0,"failed input retried or transformed");
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
    }
    std::cout<<"PNG block float bits, chunk boundaries and failure checks passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
