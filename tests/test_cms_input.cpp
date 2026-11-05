#include "cms_input.h"
#include <lcms2.h>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
#include <limits>
using namespace thumbgen::detail;
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(){try{
    auto profile=cmsCreate_sRGBProfile();require(profile!=nullptr,"fixture profile");
    cmsUInt32Number bytes=0;cmsSaveProfileToMem(profile,nullptr,&bytes);
    std::vector<unsigned char> data(bytes);cmsSaveProfileToMem(profile,data.data(),&bytes);cmsCloseProfile(profile);
    for(std::size_t limit:{0,1,128,512,1024,4096,16384,65536,1048576}) {
        CmsStats stats;
        try {CmsInput input(data.data(),data.size(),CmsModel::rgb,stats,limit);float pixel[]={.2f,.4f,.6f,.5f};input.apply(pixel,1,4,false);}
        catch(const CmsFailure&){}
        require(stats.live==0,"allocation leak after context/transform failure or release");
        require(stats.peak<=limit,"allocation ceiling exceeded");
        if(limit<1024)require(stats.denied>0,"context allocation denial missing");
    }
    for(float value:{-1.0f,2.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        CmsStats stats;bool failed=false;
        try {CmsInput input(data.data(),data.size(),CmsModel::rgb,stats);float p[]={value,.4f,.6f};input.apply(p,1,3,false);}
        catch(const CmsFailure&){failed=true;}
        require(failed && stats.live==0,"invalid normalized sample accepted or leaked");
    }
    for(std::size_t limit=cms_bootstrap_bytes;limit<cms_bootstrap_bytes+32768;limit+=256) {
        CmsStats stats;
        try{CmsInput input(data.data(),data.size(),CmsModel::rgb,stats,limit);}
        catch(const CmsFailure&){}
        require(stats.live==0 && stats.peak<=limit,"post-bootstrap allocation denial leaked or exceeded limit");
    }
    {CmsStats stats;{CmsInput input(data.data(),data.size(),CmsModel::rgb,stats);bool rejected=false;
        std::thread foreign([&](){float p[]={.2f,.4f,.6f};try{input.apply(p,1,3,false);}catch(const CmsFailure&){rejected=true;}});foreign.join();
        require(rejected,"foreign-thread processing was accepted");
    }require(stats.live==0,"thread ownership rejection leaked");}
    {CmsStats stats;{CmsInput input(data.data(),data.size(),CmsModel::rgb,stats);
        std::vector<float> pixels(8193*4);for(std::size_t i=0;i<pixels.size();i+=4){pixels[i]=.2f;pixels[i+1]=.4f;pixels[i+2]=.6f;pixels[i+3]=.5f;}
        const auto allocations=stats.allocations;input.apply(pixels.data(),8193,4,false);
        require(stats.allocations==allocations,"CMS allocates while processing blocks");
        for(std::size_t i=3;i<pixels.size();i+=4)require(pixels[i]==.5f,"block alpha changed");
    }require(stats.live==0,"block request leaked");}
    auto run=[&](){for(int i=0;i<32;++i){CmsStats stats;{CmsInput input(data.data(),data.size(),CmsModel::rgb,stats);float p[]={.2f,.4f,.6f,.5f};input.apply(p,1,4,false);require(p[3]==.5f,"alpha changed");}require(stats.live==0,"request leak");require(stats.allocations>0,"ledger inactive");}};
    std::exception_ptr failures[2];std::thread a([&](){try{run();}catch(...){failures[0]=std::current_exception();}}),b([&](){try{run();}catch(...){failures[1]=std::current_exception();}});a.join();b.join();
    for(auto failure:failures)if(failure)std::rethrow_exception(failure);
    profile=cmsCreate_sRGBProfile();require(profile!=nullptr,"request plugins corrupted global CMS context");cmsCloseProfile(profile);
    std::cout<<"CMS limit/failure cleanup and 64 concurrent independent requests passed\n"<<std::flush;
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
