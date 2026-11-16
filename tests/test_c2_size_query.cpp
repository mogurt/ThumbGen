#include <cstdlib>
#include <new>
#include <cstdio>
static bool watching=false;
static size_t new_calls=0,sampler_calls=0;
void* operator new(std::size_t n){if(watching)++new_calls;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
static void observe_sampler(size_t){if(watching)++sampler_calls;}
#define THUMBGEN_C2_ALLOCATION_WITNESS(size) observe_sampler(size)
// Compile the actual adapter in this diagnostic TU so its malloc boundary is
// observed too. The product library has no witness or global-new override.
#include "../src/c2_resize.cpp"

static void check(int w,int h,int channels,const std::vector<std::pair<int,int>>& dimensions){
    size_t bytes=0;new_calls=sampler_calls=0;watching=true;
    const bool eligible=thumbgen::detail::C2Resize::workspace_plan(w,h,channels,dimensions,bytes);
    watching=false;
    require(new_calls==0 && sampler_calls==0);
    if(eligible){
        watching=true;
        thumbgen::detail::C2Resize actual(w,h,channels,dimensions);
        watching=false;require(new_calls>0 && sampler_calls>0);
        require(bytes==actual.workspace_bytes() && bytes<=thumbgen::detail::c2_workspace_limit);
    }
}
int main(){try{
    for(int channels:{3,4})for(auto dimensions:{std::vector<std::pair<int,int>>{{425,13}},{{960,30}},{{960,30},{425,13}}})check(16384,512,channels,dimensions);
    // The known cap fallback must remain allocation-free, including operator new.
    std::vector<std::pair<int,int>> cap{{425,13}};size_t ignored;
    watching=true;new_calls=sampler_calls=0;
    const bool over=thumbgen::detail::C2Resize::workspace_plan(16384,512,4,cap,ignored);
    watching=false;require(!over && new_calls==0 && sampler_calls==0);
    unsigned seed=123456789;
    auto random=[&](){seed=1664525*seed+1013904223;return seed;};
    for(int i=0;i<384;++i){
        int w=1+int(random()%65536),h=1+int(random()%65536),channels=i%2?3:4;
        int dw=1+int(random()%std::min(w,960)),dh=1+int(random()%std::min(h,960));
        std::vector<std::pair<int,int>> dimensions{{dw,dh}};
        if(i%3)dimensions.emplace_back(1+int(random()%std::min(w,960)),1+int(random()%std::min(h,960)));
        check(w,h,channels,dimensions);
    }
    check(1,6,4,{{1,1}});check(1,65536,3,{{1,1}});check(65536,1,4,{{1,1}});
    check(65536,65536,4,{{1,1},{960,960}});check(4001,2000,4,{{425,212},{960,480}});
    check(0,100,4,{{1,1}});check(65537,100,4,{{1,1}});check(100,100,4,{});
    std::puts("398 geometry queries plus known cap rejection: zero new/malloc calls; eligible allocations equal reservations");return 0;
}catch(const std::exception& e){watching=false;std::fprintf(stderr,"%s\n",e.what());return 1;}}
