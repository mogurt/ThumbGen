#include "exr_rows.h"
#include "files.h"
#include <OpenEXR/openexr.h>
#include <OpenImageIO/imagebufalgo.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string_view>

namespace thumbgen::detail {
namespace {
constexpr int dimension_limit=65536;
constexpr int channel_limit=4096;
constexpr std::size_t metadata_limit=8*bounded_mib;
constexpr std::size_t chunk_limit=64*bounded_mib;
constexpr int chunk_count_limit=1024*1024; // OpenEXR Core's offset table limit.

// Core's allocator has no userdata. Each context is used synchronously and
// each allocation records its owner so that frees can occur outside its scope.
struct Ledger {std::size_t limit=metadata_limit,used=0;bool denied=false;};
thread_local Ledger* active_ledger=nullptr;
struct AllocationScope {
    Ledger* previous;
    explicit AllocationScope(Ledger& owner):previous(std::exchange(active_ledger,&owner)){}
    ~AllocationScope(){active_ledger=previous;}
};
struct alignas(std::max_align_t) Allocation {Ledger* owner;std::size_t bytes;};
void* allocate(std::size_t bytes) noexcept {
    if(!active_ledger)return nullptr;
    auto& ledger=*active_ledger;
    const auto available=ledger.limit-ledger.used;
    if(available<sizeof(Allocation) || bytes>available-sizeof(Allocation)) {
        ledger.denied=true;
        return nullptr;
    }
    const auto total=sizeof(Allocation)+bytes;
    void* raw=std::malloc(total);
    if(!raw)return nullptr;
    ledger.used+=total;
    return new(raw) Allocation{&ledger,total}+1;
}
void release(void* ptr) noexcept {
    if(!ptr)return;
    auto* allocation=static_cast<Allocation*>(ptr)-1;
    allocation->owner->used-=allocation->bytes;
    std::free(allocation);
}
void quiet_error(exr_const_context_t,exr_result_t,const char*) noexcept {}
struct Context {
    Ledger ledger;
    exr_context_t value=nullptr;
    Context()=default;
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;
    ~Context(){if(value)exr_finish(&value);}
    exr_result_t open(const std::string& path,int max_dimension=dimension_limit) {
        exr_context_initializer_t options=EXR_DEFAULT_CONTEXT_INITIALIZER;
        options.alloc_fn=&allocate;
        options.free_fn=&release;
        options.error_handler_fn=&quiet_error;
        options.flags=EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
        options.max_image_width=max_dimension;
        options.max_image_height=max_dimension;
        options.max_tile_width=max_dimension;
        options.max_tile_height=max_dimension;
        AllocationScope scope(ledger);
        return exr_start_read(&value,path.c_str(),&options);
    }
    void check(exr_result_t status) const {
        if(status==EXR_ERR_SUCCESS)return;
        if(ledger.denied)throw FileFailure("decode_limit_exceeded","OpenEXR decoder exceeded its reserved memory");
        throw FileFailure("decode_failed",exr_get_error_code_as_string(status));
    }
};

struct Window {
    int x,y,w,h;
    int end_x() const {return x+w;}
    int end_y() const {return y+h;}
    bool valid() const {
        return w>0 && h>0 && w<=dimension_limit && h<=dimension_limit &&
            std::int64_t(x)+w<=std::numeric_limits<int>::max() &&
            std::int64_t(y)+h<=std::numeric_limits<int>::max();
    }
    bool matches(const exr_attr_box2i_t& box) const {
        return box.min.x==x && box.min.y==y && box.max.x==end_x()-1 && box.max.y==end_y()-1;
    }
};
struct Selection {
    Window data,display;
    int part,channel_count,tile_width,tile_height,components;
    exr_pixel_type_t sample;
    std::array<std::string,4> names;
};
struct Layout {
    exr_storage_t storage;
    exr_compression_t compression;
    int block_width,block_height;
    int first_tile=0,last_tile=-1,strip_x=0,strip_width=0;
    int copy_x,copy_y,copy_end_x,copy_end_y;
    std::array<int,4> lanes{{-1,-1,-1,-1}};
    unsigned pixel_bytes,all_pixel_bytes=0;
    std::uint64_t retained_bytes=0,codec_bytes=0;
};

std::optional<Layout> inspect(exr_const_context_t context,const Selection& selected) {
    Layout layout{};
    exr_attr_box2i_t data{},display{};
    const exr_attr_chlist_t* channels=nullptr;
    int32_t chunks=0;
    const int part=selected.part;
    if(exr_get_storage(context,part,&layout.storage)!=EXR_ERR_SUCCESS ||
       exr_get_compression(context,part,&layout.compression)!=EXR_ERR_SUCCESS ||
       exr_get_data_window(context,part,&data)!=EXR_ERR_SUCCESS ||
       exr_get_display_window(context,part,&display)!=EXR_ERR_SUCCESS ||
       exr_get_chunk_count(context,part,&chunks)!=EXR_ERR_SUCCESS ||
       exr_get_channels(context,part,&channels)!=EXR_ERR_SUCCESS ||
       !selected.data.matches(data) || !selected.display.matches(display) ||
       channels->num_channels!=selected.channel_count || chunks<=0 || chunks>chunk_count_limit)return {};

    switch(layout.compression) {
    case EXR_COMPRESSION_NONE:
    case EXR_COMPRESSION_RLE:
    case EXR_COMPRESSION_ZIPS:
    case EXR_COMPRESSION_ZIP:
    case EXR_COMPRESSION_PIZ: break;
    default: return {};
    }
    if(layout.storage==EXR_STORAGE_SCANLINE) {
        int32_t lines=0;
        if(selected.tile_width || selected.tile_height ||
           exr_get_scanlines_per_chunk(context,part,&lines)!=EXR_ERR_SUCCESS ||
           lines<1 || lines>32)return {};
        layout.block_width=selected.data.w;
        layout.block_height=lines;
    } else if(layout.storage==EXR_STORAGE_TILED) {
        uint32_t tw=0,th=0;
        exr_tile_level_mode_t mode;
        exr_tile_round_mode_t round;
        if(exr_get_tile_descriptor(context,part,&tw,&th,&mode,&round)!=EXR_ERR_SUCCESS ||
           mode!=EXR_TILE_ONE_LEVEL || !tw || !th ||
           tw!=unsigned(selected.tile_width) || th!=unsigned(selected.tile_height))return {};
        layout.block_width=int(tw);
        layout.block_height=int(th);
    } else return {};

    const auto& d=selected.data;
    const auto& f=selected.display;
    const auto nx=(std::uint64_t(d.w)+layout.block_width-1)/layout.block_width;
    const auto ny=(std::uint64_t(d.h)+layout.block_height-1)/layout.block_height;
    if(std::uint64_t(chunks)!=nx*ny)return {};

    for(int c=0;c<channels->num_channels;++c) {
        const auto& channel=channels->entries[c];
        if(channel.x_sampling!=1 || channel.y_sampling!=1)return {};
        layout.all_pixel_bytes+=channel.pixel_type==EXR_PIXEL_HALF?2:4;
        for(int i=0;i<selected.components;++i) {
            if(selected.names[i]!=std::string_view(channel.name.str,channel.name.length))continue;
            if(channel.pixel_type!=selected.sample || layout.lanes[i]!=-1)return {};
            layout.lanes[i]=c;
        }
    }
    for(int i=0;i<selected.components;++i)if(layout.lanes[i]<0)return {};
    const auto sample_bytes=selected.sample==EXR_PIXEL_HALF?2:4;
    layout.pixel_bytes=selected.components*sample_bytes;
    const auto block_rows=std::min(d.h,layout.block_height);
    const auto max_chunk=std::uint64_t(std::min(d.w,layout.block_width))*block_rows*layout.all_pixel_bytes;
    if(max_chunk>chunk_limit)return {};

    layout.copy_x=std::max(d.x,f.x);
    layout.copy_end_x=std::min(d.end_x(),f.end_x());
    layout.copy_y=std::max(d.y,f.y);
    layout.copy_end_y=std::min(d.end_y(),f.end_y());
    layout.codec_bytes=metadata_limit;
    if(layout.copy_x>=layout.copy_end_x || layout.copy_y>=layout.copy_end_y)return layout;
    layout.first_tile=(layout.copy_x-d.x)/layout.block_width;
    layout.last_tile=(layout.copy_end_x-1-d.x)/layout.block_width;
    layout.strip_x=d.x+layout.first_tile*layout.block_width;
    layout.strip_width=std::min(d.w,(layout.last_tile+1)*layout.block_width)-(layout.strip_x-d.x);
    // Guard the codec's interleave detection pointer arithmetic. Only the
    // interior strip is written, using coordinates relative to its origin.
    layout.retained_bytes=std::uint64_t(layout.strip_width)*block_rows*layout.pixel_bytes+
        2*std::uint64_t(channels->num_channels)*sample_bytes;
    if(layout.retained_bytes>std::numeric_limits<std::size_t>::max())return {};
    // Core keeps only the selected part's offset table. A compressed chunk
    // can need packed, unpacked and shuffle buffers for *all* source lanes.
    // The fixed codec allowance covers channel descriptors, allocator headers,
    // libdeflate state, and both PIZ Huffman scratch tables and lookup lists.
    const auto block_copies=layout.compression==EXR_COMPRESSION_NONE?1:3;
    layout.codec_bytes+=std::uint64_t(chunks)*sizeof(std::uint64_t)+max_chunk*block_copies+4*bounded_mib;
    return layout;
}

class ExrRows final:public NativeRowSource {
    Context context_;
    Selection selection_;
    Layout layout_;
    exr_decode_pipeline_t pipeline_=EXR_DECODE_PIPELINE_INITIALIZER;
    std::vector<std::uint8_t> strip_;
    bool initialized_=false;
    int next_=0,cached_block_=-1;

    void load(int block) {
        const auto& s=selection_;
        const auto& l=layout_;
        const auto row_bytes=std::size_t(l.strip_width)*l.pixel_bytes;
        const unsigned sample_bytes=l.pixel_bytes/s.components;
        const auto guard=std::size_t(s.channel_count)*sample_bytes;
        const auto local_y=block*l.block_height;
        const auto height=std::min(l.block_height,s.data.h-local_y);
        AllocationScope scope(context_.ledger);
        for(int tile=l.first_tile;tile<=l.last_tile;++tile) {
            exr_chunk_info_t chunk{};
            const auto local_x=tile*l.block_width;
            const auto width=std::min(l.block_width,s.data.w-local_x);
            const bool tiled=l.storage==EXR_STORAGE_TILED;
            if(tiled)context_.check(exr_read_tile_chunk_info(context_.value,s.part,tile,block,0,0,&chunk));
            else context_.check(exr_read_scanline_chunk_info(context_.value,s.part,s.data.y+local_y,&chunk));
            if(chunk.start_x!=(tiled?tile:s.data.x) || chunk.start_y!=(tiled?block:s.data.y+local_y) ||
               chunk.width!=width || chunk.height!=height || chunk.type!=l.storage || chunk.compression!=l.compression ||
               chunk.unpacked_size!=std::uint64_t(width)*height*l.all_pixel_bytes ||
               !chunk.packed_size || chunk.packed_size>chunk.unpacked_size)
                throw FileFailure("decode_failed","OpenEXR chunk does not match the admitted layout");

            if(!initialized_) {
                context_.check(exr_decoding_initialize(context_.value,s.part,&chunk,&pipeline_));
                initialized_=true;
            } else context_.check(exr_decoding_update(context_.value,s.part,&chunk,&pipeline_));
            const auto offset=guard+std::size_t(local_x-(l.strip_x-s.data.x))*l.pixel_bytes;
            for(int i=0;i<s.components;++i) {
                auto& out=pipeline_.channels[l.lanes[i]];
                out.user_pixel_stride=l.pixel_bytes;
                out.user_line_stride=int32_t(row_bytes);
                out.decode_to_ptr=strip_.data()+offset+i*sample_bytes;
            }
            context_.check(exr_decoding_choose_default_routines(context_.value,s.part,&pipeline_));
            context_.check(exr_decoding_run(context_.value,s.part,&pipeline_));
        }
        cached_block_=block;
    }
public:
    ExrRows(const std::string& path,const Selection& selection,const Layout& plan):selection_(selection) {
        // Use the checked legacy ZIP pipeline ABI, as in the Deep reader.
        pipeline_.pipe_size=offsetof(exr_decode_pipeline_t,sample_count_valid);
        context_.check(context_.open(path));
        auto layout=inspect(context_.value,selection_);
        if(!layout || layout->compression!=plan.compression || layout->retained_bytes>plan.retained_bytes ||
           layout->codec_bytes>plan.codec_bytes)
            throw FileFailure("decode_failed","OpenEXR layout changed after admission");
        layout_=*layout;
        context_.ledger.limit=std::size_t(plan.codec_bytes);
        strip_.resize(std::size_t(layout_.retained_bytes));
    }
    ~ExrRows() override {
        if(initialized_)exr_decoding_destroy(context_.value,&pipeline_);
    }
    void read(int first,int last,void* destination) override {
        const auto& s=selection_;
        const auto& l=layout_;
        if(first!=next_ || first<0 || first>=last || last>s.display.h || !destination)
            throw FileFailure("decode_failed","OpenEXR rows must be read once in order");
        auto* out=static_cast<std::uint8_t*>(destination);
        const auto row_bytes=std::size_t(s.display.w)*l.pixel_bytes;
        const auto guard=std::size_t(s.channel_count)*(l.pixel_bytes/s.components);
        for(;next_<last;++next_,out+=row_bytes) {
            const int y=s.display.y+next_;
            if(l.copy_x>=l.copy_end_x || y<l.copy_y || y>=l.copy_end_y) {
                std::memset(out,0,row_bytes);
                continue;
            }
            const auto local_y=y-s.data.y;
            const auto block=local_y/l.block_height;
            if(block!=cached_block_)load(block);
            const auto input_pixel=std::size_t(local_y%l.block_height)*l.strip_width+(l.copy_x-l.strip_x);
            const auto left_bytes=std::size_t(l.copy_x-s.display.x)*l.pixel_bytes;
            const auto copy_bytes=std::size_t(l.copy_end_x-l.copy_x)*l.pixel_bytes;
            std::memset(out,0,left_bytes);
            std::memcpy(out+left_bytes,strip_.data()+guard+input_pixel*l.pixel_bytes,copy_bytes);
            std::memset(out+left_bytes+copy_bytes,0,row_bytes-left_bytes-copy_bytes);
        }
    }
    void finish() override {
        if(next_!=selection_.display.h)throw FileFailure("decode_failed","incomplete OpenEXR display rows");
    }
};
}

bool preflight_exr_header(const std::string& path) {
    constexpr std::array<unsigned char,4> signature{{0x76,0x2f,0x31,0x01}};
    std::array<unsigned char,4> magic{};
    std::ifstream input(std::filesystem::u8path(path),std::ios::binary);
    if(!input.read(reinterpret_cast<char*>(magic.data()),magic.size()) || magic!=signature)return false;
    Context context;
    context.open(path,0);
    if(context.ledger.denied)
        throw FileFailure("decode_limit_exceeded","OpenEXR metadata exceeds its allocation limit");
    return true;
}

std::optional<ExrRowProbe> probe_exr_rows(const std::string& path,const OIIO::ImageSpec& spec,
    int part,const std::vector<int>& channels) {
    if(part<0 || spec.deep || spec.depth!=1 || spec.full_depth!=1 || spec.nchannels<3 ||
       spec.nchannels>channel_limit || (channels.size()!=3 && channels.size()!=4) ||
       spec.get_int_attribute("openexr:luminancechroma",0))return {};
    Selection selection{{spec.x,spec.y,spec.width,spec.height},
        {spec.full_x,spec.full_y,spec.full_width,spec.full_height},part,spec.nchannels,
        spec.tile_width,spec.tile_height,int(channels.size()),EXR_PIXEL_HALF,{}};
    if(!selection.data.valid() || !selection.display.valid())return {};
    OIIO::TypeDesc format;
    for(int i=0;i<selection.components;++i) {
        const int c=channels[i];
        if(c<0 || c>=spec.nchannels || std::size_t(c)>=spec.channelnames.size() ||
           std::find(channels.begin(),channels.begin()+i,c)!=channels.begin()+i)return {};
        const auto native=spec.channelformat(c);
        if(native!=OIIO::TypeDesc::HALF && native!=OIIO::TypeDesc::FLOAT)return {};
        if(i && format!=native)return {};
        format=native;
        selection.names[i]=spec.channelnames[c];
    }
    selection.sample=format==OIIO::TypeDesc::HALF?EXR_PIXEL_HALF:EXR_PIXEL_FLOAT;
    Context context;
    if(context.open(path)!=EXR_ERR_SUCCESS)return {};
    const auto layout=inspect(context.value,selection);
    if(!layout)return {};
    if(layout->retained_bytes) {
        // This reads offsets for the chosen part only. Leave recoverable
        // tables to the reference decoder before bounded decode is admitted.
        context.ledger.limit=std::size_t(layout->codec_bytes);
        AllocationScope scope(context.ledger);
        if(exr_validate_chunk_table(context.value,part)!=EXR_ERR_SUCCESS)return {};
    }
    return ExrRowProbe{{layout->pixel_bytes,layout->retained_bytes,layout->codec_bytes,
        [path,selection,plan=*layout]{return std::make_unique<ExrRows>(path,selection,plan);}},
        format,selection.components,spec.get_int_attribute("oiio:UnassociatedAlpha",0)!=0};
}

void exr_rows_to_linear(const ExrRowProbe& probe,const OIIO::ColorProcessorHandle& processor,
    const void* source,float* destination,std::size_t pixels) {
    const auto components=probe.components;
    if(!OIIO::convert_pixel_values(probe.sample_format,source,OIIO::TypeDesc::FLOAT,destination,int(pixels*components)))
        throw FileFailure("decode_failed","cannot convert native OpenEXR colour block to float");
    linearize_rows(processor,destination,pixels,components,probe.unassociated_alpha);
}

void linearize_rows(const OIIO::ColorProcessorHandle& processor,float* destination,
    std::size_t pixels,int components,bool straight_alpha) {
    const bool associate=components==4 && straight_alpha;
    if(!associate && (!processor || processor->isNoOp()))return;
    // The complete-image path uses ImageBufAlgo's premult and colourconvert
    // conventions, including zero-alpha emission and RGB transforms that
    // consume a padded fourth lane. Restrict their scratch to short runs.
    for(std::size_t i=0;i<pixels;) {
        const int count=int(std::min<std::size_t>(pixels-i,2048));
        OIIO::ImageSpec spec(count,1,components,OIIO::TypeDesc::FLOAT);
        OIIO::ImageBuf image(spec,destination+i*components);
        if(associate && !OIIO::ImageBufAlgo::premult(image,image,{},1))
            throw FileFailure("alpha_failed",image.geterror());
        if(processor && !OIIO::ImageBufAlgo::colorconvert(image,image,processor.get(),true,{},1))
            throw FileFailure("colour_failed",image.geterror());
        i+=count;
    }
}
}
