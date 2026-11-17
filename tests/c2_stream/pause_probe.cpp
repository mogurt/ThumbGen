// Diagnostic only. The product still uses the unmodified locked stb path.
#include <coroutine>
#include <exception>
#include <memory>
#include <vector>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <limits>
#include <fstream>
#include <functional>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_path.h"

struct Task {
    struct promise_type {
        static inline size_t live_bytes=0,peak_bytes=0;
        static void* operator new(size_t n){auto* p=::operator new(n);live_bytes+=n;peak_bytes=std::max(peak_bytes,live_bytes);return p;}
        static void operator delete(void* p,size_t n){live_bytes-=n;::operator delete(p);}
        int row=0;
        std::exception_ptr error;
        Task get_return_object(){return Task{std::coroutine_handle<promise_type>::from_promise(*this)};}
        std::suspend_always initial_suspend() noexcept{return {};}
        std::suspend_always final_suspend() noexcept{return {};}
        std::suspend_always yield_value(int y) noexcept{row=y;return {};}
        void return_void(){}
        void unhandled_exception(){error=std::current_exception();}
    };
    std::coroutine_handle<promise_type> handle;
    explicit Task(std::coroutine_handle<promise_type> h):handle(h){}
    Task(Task&& t) noexcept:handle(t.handle){t.handle={};}
    ~Task(){if(handle)handle.destroy();}
    void next(){handle.resume();if(handle.promise().error)std::rethrow_exception(handle.promise().error);}
};
#include "paused_loops.h"

static void require(bool b){if(!b)throw std::runtime_error("probe contract failed");}
#ifdef C2_REAL_SOURCE
#include "png_source.h"
#endif
static std::function<void(int,float*)> external_row;
static std::function<void()> prepare_reference;
static std::function<void(const float*,const float*,int,int,size_t)> finish_pair;
struct InjectedStop {};
static size_t fail_after=0,cancel_after=0;
static std::ofstream trace_file;
static size_t active_consumer=0;
struct Source {
    int width,height,channels,seed,current=-1;
    size_t calls=0;
    unsigned long long row_hash=14695981039346656037ull;
    std::vector<int> rows_seen;
    std::vector<float> row;
    Source(int w,int h,int c,int s):width(w),height(h),channels(c),seed(s),row(size_t(w)*c){}
    void fill(int y,float* p) const {
        if(external_row){external_row(y,p);return;}
        for(int x=0;x<width;++x){
            unsigned v=unsigned(size_t(y)*width+x)*1664525u+unsigned(seed)*1013904223u;
            float a=seed==0?(y<3?32767:32768)*(1.0f/65535.0f):float(v>>16)*(1.0f/65535.0f);
            if(seed && seed%5==0 && x%3==0)a=0;
            for(int c=0;c<channels;++c)p[size_t(x)*channels+c]=c==3?a:(seed==0?0:a*float((v>>(c*5))&255)*(1.0f/255.0f));
        }
    }
    static void hash(unsigned long long& value,const float* data,size_t size){
        const auto* bytes=reinterpret_cast<const unsigned char*>(data);
        for(size_t i=0;i<size*4;++i){value^=bytes[i];value*=1099511628211ull;}
    }
    void advance(int y){require(y>=current);if(y!=current){current=y;fill(y,row.data());hash(row_hash,row.data(),row.size());rows_seen.push_back(y);}}
};
static const void* callback(void*,const void*,int n,int x,int y,void* ctx){
    auto& s=*static_cast<Source*>(ctx);
    require(y==s.current && x>=0 && x+n<=s.width);++s.calls;
    if(trace_file)trace_file<<"["<<active_consumer<<','<<x<<','<<y<<','<<n<<"]\n";
    if(fail_after && s.calls==fail_after)throw InjectedStop{};
    return s.row.data()+size_t(x)*s.channels;
}
static void init(STBIR_RESIZE& r,Source& s,const void* input,float* output,int w,int h){
    stbir_resize_init(&r,input,s.width,s.height,0,output,w,h,0,s.channels==4?STBIR_RGBA_PM:STBIR_RGB,STBIR_TYPE_FLOAT);
    stbir_set_filters(&r,STBIR_FILTER_TRIANGLE,STBIR_FILTER_TRIANGLE);
    stbir_set_edgemodes(&r,STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP);
}
struct Slice {
    STBIR_RESIZE resize{};
    stbir__per_split_info state{};
    std::vector<unsigned char> workspace;
    std::unique_ptr<Task> task;
    ~Slice(){task.reset();stbir_free_samplers(&resize);}
};
static bool shared=false;
static void test(int w,int h,int ow,int oh,int channels,int seed,bool dual){
    Source source(w,h,channels,seed);
    Task::promise_type::peak_bytes=Task::promise_type::live_bytes;
    if(trace_file)trace_file<<"{\"case\":["<<w<<','<<h<<','<<ow<<','<<oh<<','<<channels<<','<<seed<<','<<dual<<"]}\n";
    std::vector<std::vector<float>> outputs;
    std::vector<std::pair<int,int>> dimensions{{ow,oh}};
    if(dual)dimensions.emplace_back(std::max(1,ow/2),std::max(1,oh/2));
    outputs.resize(dimensions.size());
    std::vector<std::unique_ptr<Slice>> slices;
    size_t sampler_bytes=0;
    for(size_t k=0;k<dimensions.size();++k){
        auto [dw,dh]=dimensions[k];outputs[k].resize(size_t(dw)*dh*channels);
        int count=1;
        stbir__info* owner=nullptr;
        for(int b=0;b<count;++b){
            auto slice=std::make_unique<Slice>();auto& r=slice->resize;
            if(!shared || !owner){
                init(r,source,source.row.data(),outputs[k].data(),dw,dh);
                stbir_set_pixel_callbacks(&r,callback,nullptr);stbir_set_user_data(&r,&source);
                require(stbir_build_samplers(&r)!=0);require(r.splits==1);
                owner=r.samplers;sampler_bytes+=owner->alloced_total;
            }
            auto* info=shared?owner:r.samplers;
            count=info->num_column_slices;
            auto* state=info->split_info;
            if(shared && b){
                // Pinned allocator order: decode, ring, vertical, then tables.
                // Only this contiguous mutable workspace is duplicated. Copy
                // addresses as offsets, preserving all original page slack.
                auto* begin=static_cast<unsigned char*>(state->decode_buffer_alloc);
                auto* end=reinterpret_cast<unsigned char*>(info->horizontal.contributors);
                require(end>begin);
                const size_t bytes=size_t(end-begin);
                slice->workspace.resize(bytes+15);
                auto* base=reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(slice->workspace.data())+15)&~uintptr_t(15));
                slice->state=*state;
                auto rebase=[&](void* p)->void*{auto* q=static_cast<unsigned char*>(p);require(q>=begin && q<end);return base+(q-begin);};
                slice->state.decode_buffer_alloc=rebase(state->decode_buffer_alloc);
                slice->state.ring_buffer_alloc=rebase(state->ring_buffer_alloc);
                slice->state.vertical_buffer_alloc=rebase(state->vertical_buffer_alloc);
                state=&slice->state;sampler_bytes+=slice->workspace.capacity()+sizeof(slice->state);
            }
            stbir__4K_alias_skew_buffers(info,state);
            auto* part=info->column_slices+b;
            slice->task=std::make_unique<Task>(info->vertical.is_gather?
                pause_stbir__vertical_gather_loop(info,state,1,part):
                pause_stbir__vertical_scatter_loop(info,state,1,part));
            slice->task->next();slices.push_back(std::move(slice));
        }
    }
    size_t resumes=0;
    for(;;){
        int y=std::numeric_limits<int>::max();
        for(auto& s:slices)if(!s->task->handle.done())y=std::min(y,std::clamp(s->task->handle.promise().row,0,h-1));
        if(y==std::numeric_limits<int>::max())break;
        if(cancel_after && resumes>=cancel_after)throw InjectedStop{};
        source.advance(y);
        for(size_t i=0;i<slices.size();++i){auto& s=slices[i];if(!s->task->handle.done() && std::clamp(s->task->handle.promise().row,0,h-1)==y){active_consumer=i;s->task->next();++resumes;}}
    }
    // Reference allocation occurs only after the streaming execution is over.
    // It is not a source cache accessible by the coordinator or its callbacks.
    std::vector<float> reference_input(size_t(w)*h*channels);
    if(prepare_reference)prepare_reference();
    for(int y=0;y<h;++y)source.fill(y,reference_input.data()+size_t(y)*w*channels);
    unsigned long long reference_hash=14695981039346656037ull;
    for(int y:source.rows_seen)Source::hash(reference_hash,reference_input.data()+size_t(y)*w*channels,size_t(w)*channels);
    require(reference_hash==source.row_hash);
    for(size_t k=0;k<outputs.size();++k){
        auto [dw,dh]=dimensions[k];std::vector<float> reference(outputs[k].size());STBIR_RESIZE r{};
        init(r,source,reference_input.data(),reference.data(),dw,dh);
        require(stbir_resize_extended(&r)!=0);stbir_free_samplers(&r);
        require(std::memcmp(reference.data(),outputs[k].data(),reference.size()*sizeof(float))==0);
        if(finish_pair)finish_pair(outputs[k].data(),reference.data(),dw,dh,k);
        if(seed==0){unsigned bits;std::memcpy(&bits,&outputs[k][3],4);require(bits==0x3f000000u);}
    }
    std::cout<<"{\"input\":["<<w<<','<<h<<"],\"output\":["<<ow<<','<<oh<<"],\"channels\":"<<channels<<",\"dual\":"<<dual<<",\"shared\":"<<shared<<",\"slices\":"<<slices.size()<<",\"sampler_and_workspace_bytes\":"<<sampler_bytes<<",\"coroutine_peak_bytes\":"<<Task::promise_type::peak_bytes<<",\"source_row_bytes\":"<<source.row.size()*4<<",\"resumes\":"<<resumes<<",\"calls\":"<<source.calls<<",\"bits_equal\":true}\n";
}
int main(int argc,char** argv){try{
#ifdef C2_REAL_SOURCE
    if((argc==3 || argc==4) && std::string(argv[1])=="--png"){
        shared=true;
        if(argc==4)require(std::filesystem::create_directory(argv[3]));
        for(bool dual:{false,true}){
            PngRows source(argv[2]);std::vector<float> reference;
            external_row=[&](int y,float* p){source.get(y,p);};
            prepare_reference=[&](){
                require(source.decoded_y==source.spec.height);source.input->close();
                reference=source.full_reference(argv[2]);
                external_row=[&](int y,float* p){std::memcpy(p,reference.data()+size_t(y)*source.spec.width*source.spec.nchannels,size_t(source.spec.width)*source.spec.nchannels*sizeof(float));};
            };
            if(argc==4)finish_pair=[&](const float* candidate,const float* expected,int w,int h,size_t k){
                std::string prefix=(std::filesystem::path(argv[3])/(std::to_string(dual)+"-"+std::to_string(k))).string();
                finish_png(candidate,source.spec,w,h,prefix+"-candidate.png");finish_png(expected,source.spec,w,h,prefix+"-reference.png");
                std::ifstream a(prefix+"-candidate.png",std::ios::binary),b(prefix+"-reference.png",std::ios::binary);
                require(std::vector<char>((std::istreambuf_iterator<char>(a)),{})==std::vector<char>((std::istreambuf_iterator<char>(b)),{}));
            };
            const double scale=std::min(960.0/source.spec.width,960.0/source.spec.height);
            test(source.spec.width,source.spec.height,std::max(1,int(std::lround(source.spec.width*scale))),std::max(1,int(std::lround(source.spec.height*scale))),source.spec.nchannels,1,dual);
            external_row={};prepare_reference={};finish_pair={};
        }
        return 0;
    }
#endif
    shared=argc>1 && std::string(argv[1])=="--shared";
    if(argc>2){trace_file.open(argv[2]);require(bool(trace_file));}
    test(1,6,1,1,4,0,false);
    const int dims[][4]={{1,257,1,13},{257,1,13,1},{127,65,32,16},{4001,2000,425,212},{4001,2000,960,480},{8192,1024,960,120},{2048,4096,480,960},{7,9,7,9},{7,9,13,17}};
    for(auto& d:dims)for(int c:{3,4})for(bool dual:{false,true})test(d[0],d[1],d[2],d[3],c,1,dual);
    for(int i=1;i<=216;++i){
        int w=1+(i*7919)%513,h=1+(i*3571)%271;
        test(w,h,1+(i*13)%(w+17),1+(i*7)%(h+11),i%2?3:4,i,i%3==0);
        require(Task::promise_type::live_bytes==0);
    }
    for(int mode=0;mode<2;++mode)for(size_t at:{size_t(1),size_t(19),size_t(1000)}){
        fail_after=mode?0:at;cancel_after=mode?at:0;bool stopped=false;
        try{test(4001,2000,425,212,4,5,true);}catch(const InjectedStop&){stopped=true;}
        require(stopped && Task::promise_type::live_bytes==0);
    }
    fail_after=cancel_after=0;
    std::cout<<"{\"injected_stop_cases\":6,\"live_coroutine_bytes\":"<<Task::promise_type::live_bytes<<"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
