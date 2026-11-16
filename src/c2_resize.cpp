#include "c2_resize.h"
#include "c2_memory.h"
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <exception>
#include <limits>
#include <stdexcept>
#include <cstdlib>
namespace {
struct AllocationContext {void* owner=nullptr;size_t available=0;bool limited=false;};
void* sampler_allocate(size_t size,void* user){
    auto& context=*static_cast<AllocationContext*>(user);
    if(size>context.available){context.limited=true;return nullptr;}
    auto* memory=std::malloc(size);if(!memory)throw std::bad_alloc();
    context.available-=size;return memory;
}
}
#define STBIR_MALLOC(size,user) sampler_allocate(size,user)
#define STBIR_FREE(pointer,user) std::free(pointer)
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../third_party/stb/stb_image_resize2.h"

namespace {
// The frame limit is reserved for every slice in the plan. Compiler/layout
// changes fail closed rather than allocating beyond that reservation.
constexpr size_t frame_limit=4096;
struct Task {
    struct promise_type {
        int row=0;
        std::exception_ptr error;
        static void* operator new(size_t n){if(n>frame_limit)throw std::bad_alloc();return ::operator new(n);}
        static void operator delete(void* p,size_t){::operator delete(p);}
        Task get_return_object(){return Task{std::coroutine_handle<promise_type>::from_promise(*this)};}
        std::suspend_always initial_suspend() noexcept{return {};}
        std::suspend_always final_suspend() noexcept{return {};}
        std::suspend_always yield_value(int y) noexcept{row=y;return {};}
        void return_void(){}
        void unhandled_exception(){error=std::current_exception();}
    };
    std::coroutine_handle<promise_type> handle;
    explicit Task(std::coroutine_handle<promise_type> h):handle(h){}
    Task(Task&& other) noexcept:handle(other.handle){other.handle={};}
    ~Task(){if(handle)handle.destroy();}
    void next(){handle.resume();if(handle.promise().error)std::rethrow_exception(handle.promise().error);}
};
#include "c2_stb_loops.h"
void require(bool value){if(!value)throw std::runtime_error("C2 resizer contract failed");}
struct Owner {STBIR_RESIZE resize{};~Owner(){stbir_free_samplers(&resize);}};
struct Slice {
    stbir__info* info=nullptr;
    stbir__per_split_info state{};
    int column=0;
    size_t output=0;
    std::vector<unsigned char> workspace;
    std::unique_ptr<Task> task;
};
}
namespace thumbgen::detail {
struct C2Resize::Impl {
    AllocationContext allocation;
    int width,height,channels,current=-1;
    size_t bytes=0;
    bool used=false;
    std::vector<float> row;
    // Owners outlive every coroutine and independent slice slab.
    std::vector<std::unique_ptr<Owner>> owners;
    std::vector<std::unique_ptr<Slice>> slices;
    static const void* callback(void*,const void*,int n,int x,int y,void* data){
        auto& self=*static_cast<Impl*>(static_cast<AllocationContext*>(data)->owner);
        require(y==self.current && x>=0 && n>=0 && x<=self.width-n);
        return self.row.data()+size_t(x)*self.channels;
    }
    Impl(int w,int h,int c,const std::vector<std::pair<int,int>>& outputs):width(w),height(h),channels(c){
        require(w>0 && h>0 && (c==3 || c==4) && !outputs.empty() && outputs.size()<=2);
        allocation.owner=this;
        auto account=[&](size_t n){if(n>c2_workspace_limit-bytes)throw C2PlanLimit();bytes+=n;};
        // Pointer-vector capacity and allocator overhead are reserved up front;
        // pinned geometry has at most one slice per input column, two outputs.
        account(sizeof(Impl)+size_t(w)*c*sizeof(float)+4*size_t(w)*sizeof(void*)+4096);
        row.resize(size_t(w)*c);
        for(size_t k=0;k<outputs.size();++k){
            auto [dw,dh]=outputs[k];require(dw>0 && dh>0 && dw<=w && dh<=h);
            account(sizeof(Owner));auto owner=std::make_unique<Owner>();auto& r=owner->resize;
            stbir_resize_init(&r,row.data(),w,h,0,nullptr,dw,dh,0,c==4?STBIR_RGBA_PM:STBIR_RGB,STBIR_TYPE_FLOAT);
            stbir_set_filters(&r,STBIR_FILTER_TRIANGLE,STBIR_FILTER_TRIANGLE);
            stbir_set_edgemodes(&r,STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP);
            stbir_set_pixel_callbacks(&r,callback,nullptr);stbir_set_user_data(&r,&allocation);
            allocation.available=c2_workspace_limit-bytes;
            if(!stbir_build_samplers(&r)){if(allocation.limited)throw C2PlanLimit();throw std::runtime_error("C2 sampler build failed");}
            require(r.splits==1 && r.samplers->num_column_slices<=w);
            auto* info=r.samplers;
            // Input callbacks supply the row; zero stride avoids forming a
            // fictitious full-image pointer from the one-row allocation.
            info->input_stride_bytes=0;
            account(info->alloced_total);
            const auto initial=*info->split_info;
            auto* begin=static_cast<unsigned char*>(initial.decode_buffer_alloc);
            auto* end=reinterpret_cast<unsigned char*>(info->horizontal.contributors);
            require(end>begin);
            for(int b=0;b<info->num_column_slices;++b){
                account(sizeof(Slice)+sizeof(Task)+frame_limit+(b?size_t(end-begin)+15:0));
                auto slice=std::make_unique<Slice>();slice->info=info;slice->column=b;slice->output=k;slice->state=initial;
                if(b){
                    slice->workspace.resize(size_t(end-begin)+15);
                    auto* base=reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(slice->workspace.data())+15)&~uintptr_t(15));
                    auto rebase=[&](void* p)->void*{auto* q=static_cast<unsigned char*>(p);require(q>=begin && q<end);return base+(q-begin);};
                    slice->state.decode_buffer_alloc=rebase(initial.decode_buffer_alloc);
                    slice->state.ring_buffer_alloc=rebase(initial.ring_buffer_alloc);
                    slice->state.vertical_buffer_alloc=rebase(initial.vertical_buffer_alloc);
                }
                stbir__4K_alias_skew_buffers(info,&slice->state);
                slices.push_back(std::move(slice));
            }
            owners.push_back(std::move(owner));
        }
        require(owners.capacity()*sizeof(owners[0])+slices.capacity()*sizeof(slices[0])<=4*size_t(w)*sizeof(void*)+4096);
    }
};
C2Resize::C2Resize(int w,int h,int c,const std::vector<std::pair<int,int>>& outputs):impl_(std::make_unique<Impl>(w,h,c,outputs)){}
C2Resize::~C2Resize()=default;
size_t C2Resize::workspace_bytes()const{return impl_->bytes;}
std::vector<double> C2Resize::execute(const std::function<void(int,float*)>& read,const std::vector<float*>& outputs){
    auto& p=*impl_;require(!p.used && outputs.size()==p.owners.size());p.used=true;
    for(size_t k=0;k<outputs.size();++k){require(outputs[k]!=nullptr);p.owners[k]->resize.samplers->output_data=outputs[k];}
    std::vector<double> times(outputs.size(),0);
    for(auto& s:p.slices){
        auto* part=s->info->column_slices+s->column;
        s->task=std::make_unique<Task>(s->info->vertical.is_gather?
            pause_stbir__vertical_gather_loop(s->info,&s->state,1,part):pause_stbir__vertical_scatter_loop(s->info,&s->state,1,part));
        s->task->next();
    }
    for(;;){
        int y=std::numeric_limits<int>::max();
        for(auto& s:p.slices)if(!s->task->handle.done())y=std::min(y,std::clamp(s->task->handle.promise().row,0,p.height-1));
        if(y==std::numeric_limits<int>::max())break;
        require(y>=p.current);
        if(y!=p.current){read(y,p.row.data());p.current=y;}
        for(auto& s:p.slices)if(!s->task->handle.done() && std::clamp(s->task->handle.promise().row,0,p.height-1)==y){
            auto start=std::chrono::steady_clock::now();s->task->next();
            times[s->output]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        }
    }
    return times;
}
}
