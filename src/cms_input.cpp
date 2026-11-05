#include "cms_input.h"
#include <lcms2.h>
#include <lcms2_plugin.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <new>

static_assert(LCMS_VERSION==2200,"LittleCMS 2.20 headers required");
namespace thumbgen::detail {
namespace {
struct Ledger {
    CmsStats& stats;std::size_t limit;
    unsigned char* bootstrap=nullptr;std::size_t bootstrap_used=0;
    bool initializing=true;
    std::recursive_mutex mutex;
    std::thread::id thread=std::this_thread::get_id();
};
thread_local Ledger* active=nullptr;
struct Scope {Ledger* old;explicit Scope(Ledger& l):old(active){active=&l;}~Scope(){active=old;}};
struct alignas(std::max_align_t) Header {Ledger* owner;std::size_t bytes;bool bootstrap;};
void* allocate(cmsContext,cmsUInt32Number bytes) {
    // cmsCreateContext/cmsDeleteContext pass unregistered stack fakeContext.
    // Never dereference it or look it up through cmsGetContextUserData.
    if(!active)return nullptr;
    const auto size=sizeof(Header)+std::size_t(bytes);
    auto& s=active->stats;
    if(active->initializing) {
        const auto offset=(active->bootstrap_used+alignof(Header)-1)/alignof(Header)*alignof(Header);
        if(size>cms_bootstrap_bytes-offset){++s.denied;return nullptr;}
        auto* p=new(active->bootstrap+offset) Header{active,size,true};
        active->bootstrap_used=offset+size;++s.allocations;return p+1;
    }
    if(size>active->limit || s.live>active->limit-size){++s.denied;return nullptr;}
    auto* p=static_cast<Header*>(std::malloc(size));if(!p){++s.denied;return nullptr;}
    new(p) Header{active,size,false};s.live+=size;s.peak=std::max(s.peak,s.live);++s.allocations;
    return p+1;
}
void release(cmsContext,void* ptr) {
    if(!ptr)return;
    auto* h=static_cast<Header*>(ptr)-1;
    if(!h->bootstrap){h->owner->stats.live-=h->bytes;std::free(h);}
}
void* reallocate(cmsContext context,void* ptr,cmsUInt32Number bytes) {
    if(!ptr)return allocate(context,bytes);
    if(!bytes){release(context,ptr);return nullptr;}
    auto* h=static_cast<Header*>(ptr)-1;
    Scope scope(*h->owner);
    // Account old and new blocks simultaneously; no hidden realloc peak.
    void* out=allocate(context,bytes);if(!out)return nullptr;
    std::memcpy(out,ptr,std::min(std::size_t(bytes),h->bytes-sizeof(Header)));
    release(context,ptr);return out;
}
void log_error(cmsContext,cmsUInt32Number,const char*) {} // Never allocate in callbacks.
// One recursive lock per request, allocated before entering C. 2.20's default
// POSIX mutex destructor dereferences NULL after an allocation-denied profile
// construction. Avoid that failure mode without changing global callbacks.
void* mutex_create(cmsContext){return active?&active->mutex:nullptr;}
void mutex_destroy(cmsContext,void*){}
cmsBool mutex_lock(cmsContext,void* p){if(!p)return FALSE;try{static_cast<std::recursive_mutex*>(p)->lock();return TRUE;}catch(...){return FALSE;}}
void mutex_unlock(cmsContext,void* p){if(p)static_cast<std::recursive_mutex*>(p)->unlock();}
}
struct CmsInput::Impl {
    Ledger ledger;
    cmsContext context=nullptr;
    cmsHPROFILE input=nullptr,output=nullptr;
    cmsHTRANSFORM transform=nullptr;
    CmsModel model;
    Impl(CmsStats& stats,std::size_t limit,CmsModel m):ledger{stats,limit},model(m){
        // No quota failures may interrupt 2.20's non-transactional plugin-chunk
        // initialization (a missing chunk falls back to process-global state).
        if(limit<cms_bootstrap_bytes){++stats.denied;throw CmsFailure("CMS budget is below the 64 KiB context bootstrap minimum");}
        ledger.bootstrap=static_cast<unsigned char*>(std::malloc(cms_bootstrap_bytes));
        if(!ledger.bootstrap)throw CmsFailure("CMS context bootstrap allocation failed");
        stats.live+=cms_bootstrap_bytes;stats.peak=std::max(stats.peak,stats.live);++stats.allocations;
    }
    ~Impl(){
        Scope scope(ledger);
        if(transform)cmsDeleteTransform(transform);
        if(output)cmsCloseProfile(output);
        if(input)cmsCloseProfile(input);
        if(context)cmsDeleteContext(context);
        if(ledger.bootstrap){ledger.stats.live-=cms_bootstrap_bytes;std::free(ledger.bootstrap);}
    }
};
CmsInput::CmsInput(const void* data,std::size_t bytes,CmsModel model,CmsStats& stats,std::size_t limit)
    :impl_(std::make_unique<Impl>(stats,limit,model)) {
    if(cmsGetEncodedCMMversion()!=2200)throw CmsFailure("LittleCMS 2.20 runtime required");
    if(!data || bytes<132 || bytes>icc_profile_limit)throw CmsFailure("ICC profile size outside supported bounds");
    auto* raw=static_cast<const unsigned char*>(data);
    const std::size_t declared=(std::size_t(raw[0])<<24)|(std::size_t(raw[1])<<16)|(std::size_t(raw[2])<<8)|raw[3];
    if(declared!=bytes)throw CmsFailure("ICC profile declared size mismatch");
    Scope scope(impl_->ledger);
    cmsPluginMemHandler memory{};
    cmsPluginMutex mutex{};
    mutex.base={cmsPluginMagicNumber,LCMS_VERSION,cmsPluginMutexSig,nullptr};
    mutex.CreateMutexPtr=mutex_create;mutex.DestroyMutexPtr=mutex_destroy;mutex.LockMutexPtr=mutex_lock;mutex.UnlockMutexPtr=mutex_unlock;
    memory.base={cmsPluginMagicNumber,LCMS_VERSION,cmsPluginMemHandlerSig,&mutex.base};
    memory.MallocPtr=allocate;memory.FreePtr=release;memory.ReallocPtr=reallocate;
    impl_->context=cmsCreateContext(&memory,nullptr);
    impl_->ledger.initializing=false;
    if(!impl_->context)throw CmsFailure("CMS context allocation limit exceeded");
    cmsSetLogErrorHandlerTHR(impl_->context,log_error);
    cmsSetAdaptationStateTHR(impl_->context,1.0);
    impl_->input=cmsOpenProfileFromMemTHR(impl_->context,data,static_cast<cmsUInt32Number>(bytes));
    if(!impl_->input)throw CmsFailure("invalid or unsupported ICC profile");
    const auto kind=cmsGetDeviceClass(impl_->input);
    if(kind!=cmsSigInputClass && kind!=cmsSigDisplayClass && kind!=cmsSigOutputClass)
        throw CmsFailure("ICC device class unsupported (only input/display/output)");
    if(cmsGetColorSpace(impl_->input)!=(model==CmsModel::grey?cmsSigGrayData:model==CmsModel::cmyk?cmsSigCmykData:cmsSigRgbData))
        throw CmsFailure("ICC colour model does not match selected RGB/Gray samples");
    impl_->output=cmsCreate_sRGBProfileTHR(impl_->context);
    if(!impl_->output)throw CmsFailure("CMS sRGB profile allocation failed");
    impl_->transform=cmsCreateTransformTHR(impl_->context,impl_->input,model==CmsModel::grey?TYPE_GRAY_FLT:model==CmsModel::cmyk?TYPE_CMYK_FLT:TYPE_RGB_FLT,
        impl_->output,TYPE_RGB_FLT,INTENT_RELATIVE_COLORIMETRIC,cmsFLAGS_NOOPTIMIZE|cmsFLAGS_NOCACHE);
    if(!impl_->transform)throw CmsFailure("unsupported ICC transform or CMS memory limit exceeded");
}
CmsInput::~CmsInput()=default;
void CmsInput::apply(float* pixels,std::size_t count,int channels,bool associated) {
    if(std::this_thread::get_id()!=impl_->ledger.thread)throw CmsFailure("CMS request processing must stay on its owning thread");
    if(!pixels || (channels!=3 && channels!=4))throw CmsFailure("unsupported CMS pixel layout");
    Scope scope(impl_->ledger);
    const int components=impl_->model==CmsModel::grey?1:impl_->model==CmsModel::cmyk?4:3;
    if(components==4 && channels!=4)throw CmsFailure("CMYK needs four raw ink channels");
    std::array<float,4096*4> input{};
    std::array<float,4096*3> output{};
    for(std::size_t base=0;base<count;base+=4096) {
        const auto n=std::min<std::size_t>(4096,count-base);
        for(std::size_t i=0;i<n;++i) {
            const auto* p=pixels+(base+i)*channels;
            const float alpha=channels==4 && components!=4?p[3]:1;
            if(!std::isfinite(alpha) || alpha<0 || alpha>1)throw CmsFailure("ICC input alpha must be finite in [0,1]");
            for(int c=0;c<components;++c) {
                if(associated && alpha==0 && p[c]!=0)throw CmsFailure("ICC cannot interpret associated zero-alpha emission");
                const float v=associated?(alpha>0?p[c]/alpha:0):p[c];
                if(!std::isfinite(v) || v<0 || v>1)throw CmsFailure("ICC input requires finite normalized samples (HDR/negative unsupported)");
                input[i*components+c]=components==4?v*100:v;
            }
        }
        cmsDoTransform(impl_->transform,input.data(),output.data(),static_cast<cmsUInt32Number>(n));
        for(std::size_t i=0;i<n;++i) {
            auto* p=pixels+(base+i)*channels;
            for(int c=0;c<3;++c) {
                if(!std::isfinite(output[i*3+c]))throw CmsFailure("CMS produced non-finite RGB");
                p[c]=output[i*3+c]*(associated && channels==4?p[3]:1);
            }
            if(components==4)p[3]=1;
        }
    }
}
}
