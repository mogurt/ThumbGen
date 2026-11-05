#include "deep_reader.h"
#include "deep_flatten.h"
#include "files.h"
#include <OpenEXR/openexr.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace thumbgen::detail {
namespace {
std::size_t multiply(std::size_t a,std::size_t b){
    if(a && b>std::numeric_limits<std::size_t>::max()/a)throw DeepFailure("deep_decode_limit_exceeded","Deep byte size overflows");
    return a*b;
}
struct Ledger {
    const std::size_t limit;
    std::atomic<std::size_t> used{0},peak{0};std::atomic<bool> denied{false};
    bool take(std::size_t bytes) noexcept {
        auto previous=used.load();
        do{if(previous>limit || bytes>limit-previous){denied=true;return false;}}
        while(!used.compare_exchange_weak(previous,previous+bytes));
        auto high=peak.load();while(high<previous+bytes && !peak.compare_exchange_weak(high,previous+bytes)){}
        return true;
    }
    void put(std::size_t bytes) noexcept {used.fetch_sub(bytes);}
};
thread_local Ledger* current_ledger=nullptr;
struct AllocationScope {Ledger* previous;explicit AllocationScope(Ledger& ledger):previous(current_ledger){current_ledger=&ledger;}~AllocationScope(){current_ledger=previous;}};
struct alignas(std::max_align_t) Allocation {Ledger* ledger;std::size_t bytes;};
void* core_allocate(std::size_t bytes) noexcept {
    auto* ledger=current_ledger;if(!ledger || bytes>std::numeric_limits<std::size_t>::max()-sizeof(Allocation))return nullptr;
    const auto total=bytes+sizeof(Allocation);if(!ledger->take(total))return nullptr;
    void* raw=std::malloc(total);if(!raw){ledger->put(total);return nullptr;}
    auto* allocation=new(raw) Allocation{ledger,total};return allocation+1;
}
void core_free(void* pointer) noexcept {
    if(!pointer)return;auto* allocation=static_cast<Allocation*>(pointer)-1;
    allocation->ledger->put(allocation->bytes);std::free(allocation);
}
class Reservation {
    Ledger& ledger_;std::size_t bytes_=0;
public:
    explicit Reservation(Ledger& ledger):ledger_(ledger){}
    ~Reservation(){ledger_.put(bytes_);}
    void grow(std::size_t bytes){
        if(bytes<=bytes_)return;
        if(!ledger_.take(bytes-bytes_))throw DeepFailure("deep_decode_limit_exceeded","Deep combined buffer budget exceeded");
        bytes_=bytes;
    }
};
struct Input {
#ifdef _WIN32
    HANDLE stream=INVALID_HANDLE_VALUE;
#else
    std::FILE* stream=nullptr;
#endif
    std::uint64_t size;std::mutex mutex;
    explicit Input(const std::filesystem::path& path):size(std::filesystem::file_size(path)){
        if(size>std::uint64_t(std::numeric_limits<std::int64_t>::max()))throw DeepFailure("deep_decode_failed","Deep source is too large");
#ifdef _WIN32
        stream=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,nullptr);
        if(stream==INVALID_HANDLE_VALUE)throw DeepFailure("deep_decode_failed","Cannot open Deep source");
#else
        stream=std::fopen(path.c_str(),"rb");
        if(!stream)throw DeepFailure("deep_decode_failed","Cannot open Deep source");
        // A second count pass must observe actual file reads, not stale bytes
        // retained by the standard stream's read-ahead buffer.
        if(std::setvbuf(stream,nullptr,_IONBF,0)!=0){std::fclose(stream);stream=nullptr;throw DeepFailure("deep_decode_failed","Cannot configure Deep input stream");}
#endif
    }
    ~Input(){
#ifdef _WIN32
        if(stream!=INVALID_HANDLE_VALUE)CloseHandle(stream);
#else
        if(stream)std::fclose(stream);
#endif
    }
};
std::int64_t input_size(exr_const_context_t,void* data) noexcept {return std::int64_t(static_cast<Input*>(data)->size);}
std::int64_t input_read(exr_const_context_t,void* data,void* buffer,std::uint64_t size,std::uint64_t offset,exr_stream_error_func_ptr_t) noexcept {
    try {
        auto& input=*static_cast<Input*>(data);std::lock_guard<std::mutex> guard(input.mutex);
        if(offset>input.size || size>input.size-offset || size>std::numeric_limits<std::size_t>::max())return -1;
#ifdef _WIN32
        LARGE_INTEGER position;position.QuadPart=std::int64_t(offset);if(!SetFilePointerEx(input.stream,position,nullptr,FILE_BEGIN))return -1;
        std::uint64_t done=0;
        while(done<size){const auto wanted=DWORD(std::min<std::uint64_t>(size-done,0x7fffffff));DWORD actual=0;
            if(!ReadFile(input.stream,static_cast<unsigned char*>(buffer)+std::size_t(done),wanted,&actual,nullptr) || actual!=wanted)return -1;
            done+=actual;
        }
        return std::int64_t(size);
#else
        if(fseeko(input.stream,off_t(offset),SEEK_SET)!=0)return -1;
        return std::fread(buffer,1,std::size_t(size),input.stream)==size?std::int64_t(size):-1;
#endif
    }catch(...){return -1;}
}
void quiet_error(exr_const_context_t,exr_result_t,const char*) noexcept {}
void checked(exr_result_t status,const Ledger& ledger){
    if(status==EXR_ERR_SUCCESS)return;
    if(ledger.denied)throw DeepFailure("deep_decode_limit_exceeded","OpenEXR allocation exceeds combined Deep budget");
    throw DeepFailure("deep_decode_failed",exr_get_error_code_as_string(status));
}
struct Context {exr_context_t value=nullptr;~Context(){if(value)exr_finish(&value);}};
struct Pipeline {
    exr_const_context_t context;exr_decode_pipeline_t value=EXR_DECODE_PIPELINE_INITIALIZER;
    explicit Pipeline(exr_const_context_t c):context(c){
        // Select Core's supported legacy pipeline ABI. In the locked 3.5
        // runtimes the new cached ZIP-context path dereferences a failed
        // allocation; the legacy exr_uncompress_buffer path checks it.
        // Keep the full object allocated, including the newer bookkeeping.
        value.pipe_size=offsetof(exr_decode_pipeline_t,sample_count_valid);
    }
    ~Pipeline(){exr_decoding_destroy(context,&value);}
};
struct Plan {
    std::array<const std::string*,8> names{};std::array<int,8> mapping{};std::size_t count=0,source_bytes=0;
};
struct CountsGuard {const std::int32_t* counts;std::size_t bytes;};
exr_result_t unchanged_counts(exr_decode_pipeline_t* pipeline) noexcept {
    const auto& guard=*static_cast<CountsGuard*>(pipeline->decoding_user_data);
    return std::memcmp(guard.counts,pipeline->sample_count_table,guard.bytes)==0?EXR_ERR_SUCCESS:EXR_ERR_INVALID_SAMPLE_DATA;
}
Plan make_plan(exr_const_context_t context,int part,const DeepChannels& wanted,const Ledger& ledger){
    const exr_attr_chlist_t* channels=nullptr;checked(exr_get_channels(context,part,&channels),ledger);
    if(channels->num_channels<=0 || channels->num_channels>4096)throw DeepFailure("deep_layout_unsupported","Deep channel count is unsupported");
    Plan plan;
    for(int c=0;c<channels->num_channels;++c){const auto& channel=channels->entries[c];
        if(channel.x_sampling!=1 || channel.y_sampling!=1)throw DeepFailure("deep_layout_unsupported","Subsampled Deep channels are unsupported");
        plan.source_bytes+=channel.pixel_type==EXR_PIXEL_HALF?2:4;
    }
    for(int slot=0;slot<8;++slot){
        const auto& name=slot<3?wanted.colour[slot]:slot<6?wanted.alpha[slot-3]:slot==6?wanted.z:wanted.zback;
        if(name.empty()){
            if(slot<3 || slot==6)throw DeepFailure("deep_layout_unsupported","Deep colour and Z channels must be explicit");
            plan.mapping[slot]=-1;continue;
        }
        if(name.size()>255)throw DeepFailure("deep_layout_unsupported","Deep channel name is too long");
        int index=-1;
        for(int c=0;c<channels->num_channels;++c)if(name==channels->entries[c].name.str){
            if(channels->entries[c].pixel_type!=EXR_PIXEL_HALF && channels->entries[c].pixel_type!=EXR_PIXEL_FLOAT)
                throw DeepFailure("deep_layout_unsupported","Selected Deep channels require HALF or FLOAT");
            index=c;break;
        }
        if(index<0)throw DeepFailure("deep_layout_unsupported","Selected Deep channel is missing");
        std::size_t compact=0;while(compact<plan.count && *plan.names[compact]!=name)++compact;
        if(compact==plan.count)plan.names[plan.count++]=&name;
        plan.mapping[slot]=int(compact);
    }
    return plan;
}
}
DeepImage read_deep_image(const std::filesystem::path& path,int part,const DeepChannels& channels,DeepReadLimits limits,DeepCheckpoint checkpoint){
    if(part<0 || !limits.bytes || limits.samples_per_pixel>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Invalid Deep read limits or part");
    const auto stamp=source_stamp(path);
    auto verify=[&]{if(!source_matches(path,stamp))throw FileFailure("source_changed","Deep source identity, size or mtime changed",true);};
    auto progress=[&](const char* phase){if(checkpoint)checkpoint(phase);verify();};
    Ledger ledger{limits.bytes};DeepImage image;
    try {
        {
            AllocationScope allocation_scope(ledger);Input input(path);Context context;
            exr_context_initializer_t init=EXR_DEFAULT_CONTEXT_INITIALIZER;
            init.alloc_fn=core_allocate;init.free_fn=core_free;init.user_data=&input;init.read_fn=input_read;init.size_fn=input_size;init.error_handler_fn=quiet_error;
            init.flags|=EXR_CONTEXT_FLAG_STRICT_HEADER;
            init.max_image_width=1<<20;init.max_image_height=1<<20;init.max_tile_width=1<<16;init.max_tile_height=1<<16;
            progress("open");checked(exr_start_read(&context.value,path.u8string().c_str(),&init),ledger);
            exr_storage_t storage;checked(exr_get_storage(context.value,part,&storage),ledger);
            if(storage!=EXR_STORAGE_DEEP_SCANLINE && storage!=EXR_STORAGE_DEEP_TILED)throw DeepFailure("deep_layout_unsupported","Selected part is not Deep");
            exr_compression_t compression;checked(exr_get_compression(context.value,part,&compression),ledger);
            if(compression!=EXR_COMPRESSION_NONE && compression!=EXR_COMPRESSION_RLE && compression!=EXR_COMPRESSION_ZIPS)
                throw DeepFailure("deep_layout_unsupported","Deep compression needs a separately bounded decoder");
            exr_attr_box2i_t window;checked(exr_get_data_window(context.value,part,&window),ledger);
            const auto width=std::int64_t(window.max.x)-window.min.x+1,height=std::int64_t(window.max.y)-window.min.y+1;
            if(width<=0 || height<=0 || width>(1<<20) || height>(1<<20))throw DeepFailure("deep_decode_limit_exceeded","Deep dimensions exceed reader limits");
            image.x=window.min.x;image.y=window.min.y;image.width=int(width);image.height=int(height);
            const auto plan=make_plan(context.value,part,channels,ledger);
            image.bytes=multiply(multiply(std::size_t(width),std::size_t(height)),6*sizeof(float));
            Reservation output_memory(ledger),kernel_memory(ledger);output_memory.grow(image.bytes);
            image.pixels.reset(new float[image.bytes/sizeof(float)]{});
            DeepFlattener kernel({limits.samples_per_pixel,limits.bytes});
            auto block=[&](const exr_chunk_info_t& chunk,std::int64_t origin_x,std::int64_t origin_y){
                progress("chunk");
                if(chunk.width<=0 || chunk.height<=0 || origin_x<window.min.x || origin_y<window.min.y ||
                   origin_x+chunk.width>std::int64_t(window.max.x)+1 || origin_y+chunk.height>std::int64_t(window.max.y)+1)
                    throw DeepFailure("deep_decode_failed","Invalid Deep chunk geometry");
                if(chunk.packed_size>limits.bytes || chunk.unpacked_size>limits.bytes || chunk.sample_count_table_size>limits.bytes)
                    throw DeepFailure("deep_decode_limit_exceeded","Declared Deep chunk bytes exceed budget");
                if(chunk.data_offset>input.size || chunk.packed_size>input.size-chunk.data_offset ||
                   chunk.sample_count_data_offset>input.size || chunk.sample_count_table_size>input.size-chunk.sample_count_data_offset)
                    throw DeepFailure("deep_decode_failed","Deep chunk exceeds source file");
                Pipeline pipeline(context.value);auto& pipe=pipeline.value;checked(exr_decoding_initialize(context.value,part,&chunk,&pipe),ledger);
                pipe.decode_flags=EXR_DECODE_SAMPLE_COUNTS_AS_INDIVIDUAL|EXR_DECODE_SAMPLE_DATA_ONLY;
                checked(exr_decoding_choose_default_routines(context.value,part,&pipe),ledger);checked(exr_decoding_run(context.value,part,&pipe),ledger);
                const auto pixels=multiply(std::size_t(chunk.width),std::size_t(chunk.height));std::size_t samples=0,maximum=0;
                for(std::size_t pixel=0;pixel<pixels;++pixel){
                    const auto count=pipe.sample_count_table[pixel];
                    if(count<0)throw DeepFailure("deep_decode_failed","Negative Deep sample count");
                    if(std::size_t(count)>limits.samples_per_pixel)throw DeepFailure("deep_sample_limit_exceeded","Deep pixel sample limit exceeded");
                    if(std::uint64_t(count)>limits.samples_per_chunk-samples)throw DeepFailure("deep_sample_limit_exceeded","Deep chunk sample limit exceeded");
                    samples+=std::size_t(count);maximum=std::max(maximum,std::size_t(count));
                }
                if(pipe.sample_count_table[pixels]<0 || std::size_t(pipe.sample_count_table[pixels])!=samples || chunk.unpacked_size!=multiply(samples,plan.source_bytes))
                    throw DeepFailure("deep_decode_failed","Deep counts and declared sample bytes disagree");
                if(samples>limits.samples_per_image-image.samples)throw DeepFailure("deep_sample_limit_exceeded","Deep image sample limit exceeded");
                progress("counts"); // A controller may cancel before any sample allocation/read.
                Reservation plane_memory(ledger),sample_memory(ledger),count_memory(ledger);
                plane_memory.grow(multiply(multiply(samples,plan.count),sizeof(float)));
                sample_memory.grow(multiply(maximum,sizeof(DeepSample)));
                kernel_memory.grow(DeepFlattener::workspace_bound(maximum));
                std::unique_ptr<float[]> planes(samples?new float[multiply(samples,plan.count)]:nullptr);
                std::unique_ptr<DeepSample[]> pixel_samples(maximum?new DeepSample[maximum]:nullptr);
                std::unique_ptr<std::int32_t[]> original_counts;
                CountsGuard guard{};
                if(samples){
                    count_memory.grow(multiply(pixels+1,sizeof(std::int32_t)));
                    original_counts.reset(new std::int32_t[pixels+1]);guard={original_counts.get(),multiply(pixels+1,sizeof(std::int32_t))};
                    std::memcpy(original_counts.get(),pipe.sample_count_table,guard.bytes);
                    for(int c=0;c<pipe.channel_count;++c){auto& channel=pipe.channels[c];channel.decode_to_ptr=nullptr;
                        for(std::size_t compact=0;compact<plan.count;++compact)if(*plan.names[compact]==channel.channel_name){
                            channel.user_data_type=EXR_PIXEL_FLOAT;channel.user_bytes_per_element=sizeof(float);
                            channel.decode_to_ptr=reinterpret_cast<std::uint8_t*>(planes.get()+compact*samples);break;
                        }
                    }
                    pipe.decode_flags=EXR_DECODE_SAMPLE_COUNTS_AS_INDIVIDUAL;checked(exr_decoding_choose_default_routines(context.value,part,&pipe),ledger);
                    pipe.decoding_user_data=&guard;pipe.realloc_nonimage_data_fn=unchanged_counts;
                    checked(exr_decoding_run(context.value,part,&pipe),ledger);
                }
                progress("decoded_chunk");std::size_t offset=0;
                auto value=[&](int slot,std::size_t s,float missing){return plan.mapping[slot]<0?missing:planes[std::size_t(plan.mapping[slot])*samples+offset+s];};
                for(int y=0;y<chunk.height;++y)for(int x=0;x<chunk.width;++x){
                    auto count=std::size_t(pipe.sample_count_table[std::size_t(y)*chunk.width+x]);
                    if(count>maximum || count>samples-offset)throw DeepFailure("deep_decode_failed","Deep sample counts changed during decode");
                    for(std::size_t s=0;s<count;++s){auto& sample=pixel_samples[s];sample.z=value(6,s,0);sample.zback=value(7,s,sample.z);
                        for(int c=0;c<3;++c){sample.colour[c]=value(c,s,0);sample.alpha[c]=value(c+3,s,1);}
                    }
                    auto flat=kernel.flatten(pixel_samples.get(),count);
                    auto destination=(std::size_t(origin_y+y-window.min.y)*image.width+std::size_t(origin_x+x-window.min.x))*6;
                    for(int c=0;c<3;++c){auto colour=float(flat.colour[c]);if(!std::isfinite(colour))throw DeepFailure("deep_value_invalid","Flattened Deep colour exceeds FLOAT range");image.pixels[destination+c]=colour;image.pixels[destination+3+c]=float(flat.alpha[c]);}
                    offset+=count;
                }
                if(offset!=samples)throw DeepFailure("deep_decode_failed","Deep sample total changed during decode");
                image.samples+=samples;++image.chunks;
            };
            if(storage==EXR_STORAGE_DEEP_TILED){
                std::int32_t nx=0,ny=0,tw=0,th=0;
                checked(exr_get_tile_counts(context.value,part,0,0,&nx,&ny),ledger);checked(exr_get_tile_sizes(context.value,part,0,0,&tw,&th),ledger);
                if(nx<=0 || ny<=0 || tw<=0 || th<=0 || nx!=(width+tw-1)/tw || ny!=(height+th-1)/th)throw DeepFailure("deep_decode_failed","Deep tile coverage is invalid");
                for(int y=0;y<ny;++y)for(int x=0;x<nx;++x){progress("chunk_info");exr_chunk_info_t chunk{};checked(exr_read_tile_chunk_info(context.value,part,x,y,0,0,&chunk),ledger);block(chunk,std::int64_t(window.min.x)+std::int64_t(x)*tw,std::int64_t(window.min.y)+std::int64_t(y)*th);}
            }else{
                for(std::int64_t y=window.min.y;y<=window.max.y;){progress("chunk_info");exr_chunk_info_t chunk{};checked(exr_read_scanline_chunk_info(context.value,part,int(y),&chunk),ledger);block(chunk,window.min.x,y);y+=chunk.height;}
            }
            progress("complete");
        }
        if(ledger.used!=0)throw DeepFailure("deep_decode_failed","Deep allocator did not release all reservations");
        image.peak_accounted_bytes=ledger.peak;verify();return image;
    }catch(...){
        image.pixels.reset();
        if(ledger.used!=0)throw DeepFailure("deep_decode_failed","Deep allocator retained buffers after rejection");
        verify();throw;
    }
}
}
