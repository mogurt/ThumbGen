#include "psd_rows.h"
#include "cms_input.h"
#include "files.h"
#include <zlib.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>

namespace thumbgen::detail {
namespace {
using Byte=unsigned char;
using Offset=std::uint64_t;
constexpr unsigned max_dimension=30000;
constexpr unsigned max_channels=56;
constexpr std::size_t metadata_limit=4*bounded_mib;
constexpr std::size_t zip_chunk=32*1024;
constexpr std::size_t inflate_bytes=64*1024;
constexpr std::size_t rle_entries=256;

enum Compression : unsigned { raw=0, rle=1, zip=2, predicted_zip=3 };

void require(bool valid,const char* code,const char* message) {
    if(!valid)throw FileFailure(code,message);
}

void admit(Offset bytes,Offset budget,const char* message) {
    require(bytes<=budget,"decode_limit_exceeded",message);
}

unsigned be16(const Byte* data) {
    return (unsigned(data[0])<<8)|data[1];
}

std::uint32_t be32(const Byte* data) {
    return (std::uint32_t(be16(data))<<16)|be16(data+2);
}

struct Section {
    Offset offset=0,size=0;
    Offset end() const {return offset+size;}
};

class Source {
public:
    Source(const std::string& path,Offset size,const char* error)
        :file_(std::filesystem::u8path(path),std::ios::binary),size_(size),error_(error) {}

    bool opened() const {return file_.is_open();}
    Offset size() const {return size_;}
    void read(Offset offset,void* out,std::size_t bytes) {
        require(offset<=size_ && bytes<=size_-offset &&
            offset<=Offset(std::numeric_limits<std::streamoff>::max()),
            error_,"PSD read exceeds source range");
        if(position_!=offset)file_.seekg(static_cast<std::streamoff>(offset));
        require(bool(file_.read(static_cast<char*>(out),static_cast<std::streamsize>(bytes))),
            error_,"could not read PSD source");
        position_=offset+bytes;
    }

private:
    std::ifstream file_;
    Offset size_,position_=0;
    const char* error_;
};

class Cursor {
public:
    Cursor(Source& source,Section section):source_(source),position_(section.offset),end_(section.end()) {}
    Offset remaining() const {return end_-position_;}

    Section take(Offset bytes) {
        require(bytes<=remaining(),"open_failed","PSD block exceeds enclosing section");
        const Section section{position_,bytes};
        position_+=bytes;
        return section;
    }

    template<std::size_t N> std::array<Byte,N> read() {
        std::array<Byte,N> bytes;
        source_.read(take(N).offset,bytes.data(),N);
        return bytes;
    }

    Section sized_section() {return take(be32(read<4>().data()));}

private:
    Source& source_;
    Offset position_,end_;
};

struct Resources {
    Section background,icc,exif1,exif3;
    bool missing_composite=false;
};

Resources resources(Source& source,Section section) {
    Resources result;
    Cursor blocks(source,section);
    while(blocks.remaining()) {
        const auto header=blocks.read<7>();
        require(std::memcmp(header.data(),"8BIM",4)==0,"open_failed","invalid PSD image resource signature");
        // The Pascal length byte is already consumed. The whole name is even-aligned.
        const unsigned name_length=header[6];
        blocks.take(name_length+((name_length+1)&1));
        const auto payload=blocks.sized_section();
        blocks.take(payload.size&1);
        switch(be16(header.data()+4)) {
        case 1010: if(!result.background.offset)result.background=payload; break;
        case 1039: if(!result.icc.offset)result.icc=payload; break;
        case 1057: {
            Cursor version(source,payload);
            const auto info=version.read<5>();
            require(info[4]<=1,"open_failed","invalid PSD merged image flag");
            result.missing_composite|=info[4]==0;
            break;
        }
        case 1058: if(!result.exif1.offset)result.exif1=payload; break;
        case 1059: if(!result.exif3.offset)result.exif3=payload; break;
        }
    }
    return result;
}

bool negative_layer_count(Source& source,Section section) {
    Cursor layers(source,section);
    const auto count=be16(layers.read<2>().data());
    require(count!=0x8000,"open_failed","invalid PSD layer count");
    return (count&0x8000)!=0;
}

bool merged_alpha(Source& source,Section section,unsigned sample_bytes) {
    if(!section.size)return false;
    Cursor masks(source,section);
    const auto layers=masks.sized_section();
    bool alpha=layers.size && negative_layer_count(source,layers);
    if(!masks.remaining())return alpha; // Some writers omit the empty global mask section.
    masks.sized_section(); // Global layer mask data.
    bool have_layers=layers.size!=0;
    while(masks.remaining()>=12) {
        const auto tag=masks.read<8>();
        require(std::memcmp(tag.data(),"8BIM",4)==0 || std::memcmp(tag.data(),"8B64",4)==0,
            "open_failed","invalid PSD additional layer info signature");
        const auto payload=masks.sized_section();
        const char* layer_key=sample_bytes==2?"Lr16":"Layr";
        if(std::memcmp(tag.data()+4,layer_key,4)==0) {
            require(!have_layers,"open_failed","duplicate PSD layer info");
            alpha=negative_layer_count(source,payload);
            have_layers=true;
        }
        // PSD global tagged blocks are commonly padded to a multiple of four.
        // The enclosing section may end directly after the final payload.
        if(masks.remaining())masks.take((4-(payload.size&3))&3);
    }
    require(masks.remaining()<=3,"open_failed","truncated PSD additional layer info header");
    while(masks.remaining())
        require(masks.read<1>()[0]==0,"open_failed","invalid PSD layer info padding");
    return alpha;
}

int exif_orientation(Source& source,Section exif) {
    if(exif.size<8)return 0;
    std::array<Byte,8> header;
    source.read(exif.offset,header.data(),header.size());
    if(std::memcmp(header.data(),"Exif\0\0",6)==0 && exif.size>=14) {
        exif.offset+=6;
        exif.size-=6;
        source.read(exif.offset,header.data(),header.size());
    }
    const bool little=header[0]=='I' && header[1]=='I';
    if(!little && !(header[0]=='M' && header[1]=='M'))return 0;
    const auto u16=[little](const Byte* bytes) {return little?unsigned(bytes[0])|(unsigned(bytes[1])<<8):be16(bytes);};
    const auto u32=[little,u16](const Byte* bytes) {
        return little?std::uint32_t(u16(bytes))|(std::uint32_t(u16(bytes+2))<<16):be32(bytes);
    };
    const Offset ifd=u32(header.data()+4);
    if(u16(header.data()+2)!=42 || ifd<8 || ifd>exif.size-2)return 0;
    Cursor entries(source,{exif.offset+ifd,exif.size-ifd});
    const auto count=u16(entries.read<2>().data());
    if(Offset(count)*12+4>entries.remaining())return 0;
    for(unsigned i=0;i<count;++i) {
        const auto entry=entries.read<12>();
        if(u16(entry.data())!=274 || u16(entry.data()+2)!=3 || u32(entry.data()+4)!=1)continue;
        const auto orientation=u16(entry.data()+8);
        if(orientation>=1 && orientation<=8)return int(orientation);
    }
    return 0;
}

void add_orientation(Source& source,const Resources& metadata,OIIO::ImageSpec& spec) {
    // The locked OIIO XMP decoder treats tiff:Orientation as redundant.
    int orientation=exif_orientation(source,metadata.exif3);
    if(!orientation)orientation=exif_orientation(source,metadata.exif1);
    if(orientation)spec.attribute("Orientation",orientation);
}

struct Layout {
    Offset size=0,payload=0;
    unsigned width=0,height=0,sample_bytes=0,planes=0,channels=0,compression=0;
    std::array<float,3> background{1.0f,1.0f,1.0f};

    unsigned row_bytes() const {return width*sample_bytes;}
    Offset plane_bytes() const {return Offset(row_bytes())*height;}
    Offset rle_table_bytes() const {return Offset(height)*planes*2;}
};

void unpack_row(const Byte* packed,std::size_t length,Byte* row,std::size_t row_bytes) {
    std::size_t in=0,out=0;
    while(in<length) {
        const unsigned command=packed[in++];
        if(command==128)continue; // PackBits no-op, including at row end.
        const auto count=command<128?command+1:257-command;
        require(count<=row_bytes-out,"decode_failed","PSD PackBits run crosses scanline");
        if(command<128) {
            require(count<=length-in,"decode_failed","truncated PSD PackBits literals");
            std::memcpy(row+out,packed+in,count);
            in+=count;
        } else {
            require(in<length,"decode_failed","truncated PSD PackBits repeat");
            std::memset(row+out,packed[in++],count);
        }
        out+=count;
    }
    require(out==row_bytes,"decode_failed","incomplete PSD PackBits scanline");
}

class RleRows {
public:
    RleRows(Source& source,const Layout& layout):source_(source),layout_(layout),packed_(65535) {
        Offset offset=layout.payload+layout.rle_table_bytes();
        std::array<Byte,2*rle_entries> slab;
        for(unsigned plane=0;plane<layout.planes;++plane) {
            positions_[plane]=offset;
            for(unsigned y=0;y<layout.height;y+=rle_entries) {
                const auto count=std::min<unsigned>(rle_entries,layout.height-y);
                read_lengths(plane,y,slab.data(),count);
                for(unsigned row=0;row<count;++row) {
                    const auto length=be16(slab.data()+2*row);
                    require(length && length<=layout.size-offset,"decode_failed","invalid PSD PackBits row length");
                    offset+=length;
                }
            }
            ends_[plane]=offset;
        }
        require(offset==layout.size,"decode_failed","unexpected bytes after PSD PackBits composite");
    }

    void read(unsigned plane,unsigned y,Byte* row) {
        // Visible planes are interleaved; one slot is reused for all ignored planes.
        auto& slab=lengths_[std::min(plane,layout_.channels)];
        if(y%rle_entries==0) {
            const auto count=std::min<unsigned>(rle_entries,layout_.height-y);
            read_lengths(plane,y,slab.data(),count);
        }
        const auto count=be16(slab.data()+2*(y%rle_entries));
        auto& offset=positions_[plane];
        require(count && count<=ends_[plane]-offset,"decode_failed","PSD PackBits row crosses plane boundary");
        source_.read(offset,packed_.data(),count);
        unpack_row(packed_.data(),count,row,layout_.row_bytes());
        offset+=count;
        if(y+1==layout_.height)
            require(offset==ends_[plane],"decode_failed","PSD PackBits plane length changed during read");
    }

private:
    void read_lengths(unsigned plane,unsigned y,Byte* slab,unsigned count) {
        source_.read(layout_.payload+2*(Offset(plane)*layout_.height+y),slab,2*count);
    }
    Source& source_;
    const Layout& layout_;
    std::vector<Byte> packed_;
    std::array<Offset,max_channels> positions_{},ends_{};
    std::array<std::array<Byte,2*rle_entries>,5> lengths_{};
};

struct InflateMemory {
    Offset live=0,limit=0;

    struct alignas(std::max_align_t) Allocation {Offset size;};

    static voidpf allocate(voidpf context,uInt count,uInt size) {
        auto& memory=*static_cast<InflateMemory*>(context);
        const Offset bytes=Offset(count)*size;
        if(bytes>memory.limit-memory.live || sizeof(Allocation)>memory.limit-memory.live-bytes)return nullptr;
        const Offset cost=bytes+sizeof(Allocation);
        auto* block=static_cast<Allocation*>(std::malloc(static_cast<std::size_t>(cost)));
        if(!block)return nullptr;
        ::new (block) Allocation{cost};
        memory.live+=cost;
        return block+1;
    }

    static void release(voidpf context,voidpf address) {
        if(!address)return;
        auto* block=static_cast<Allocation*>(address)-1;
        static_cast<InflateMemory*>(context)->live-=block->size;
        std::free(block);
    }
};

class Inflater {
public:
    Inflater()=default;
    Inflater(const Inflater&)=delete;
    Inflater& operator=(const Inflater&)=delete;
    ~Inflater() {if(active_)inflateEnd(&stream_);}

    void start(Offset offset,InflateMemory& memory) {
        stream_.zalloc=InflateMemory::allocate;
        stream_.zfree=InflateMemory::release;
        stream_.opaque=&memory;
        check(inflateInit(&stream_));
        active_=true;
        next_offset_=offset;
    }

    void resume(Inflater& earlier) {
        require(!earlier.ended_,"decode_failed","PSD ZIP output ends before all planes");
        check(inflateCopy(&stream_,&earlier.stream_));
        active_=true;
        // The copied bit accumulator and history belong to this inflater. Feed
        // its unread bytes from the file so it never references another buffer.
        next_offset_=earlier.next_offset_-earlier.stream_.avail_in;
        stream_.next_in=nullptr;
        stream_.avail_in=0;
        stream_.next_out=nullptr;
        stream_.avail_out=0;
    }

    void read(Source& source,Byte* destination,unsigned count) {
        stream_.next_out=destination;
        stream_.avail_out=count;
        while(stream_.avail_out && !ended_)step(source);
        require(stream_.avail_out==0,"decode_failed","PSD ZIP output is shorter than its composite layout");
    }

    void skip(Source& source,Byte* scratch,unsigned capacity,Offset count) {
        while(count) {
            const auto chunk=static_cast<unsigned>(std::min<Offset>(capacity,count));
            read(source,scratch,chunk);
            count-=chunk;
        }
    }

    void finish(Source& source) {
        Byte extra;
        stream_.next_out=&extra;
        stream_.avail_out=1;
        while(!ended_ && stream_.avail_out)step(source);
        require(ended_ && stream_.avail_out==1,"decode_failed","PSD ZIP output is longer than its composite layout");
        require(next_offset_-stream_.avail_in==source.size(),"decode_failed","unexpected bytes after PSD ZIP composite");
    }

private:
    static void check(int code) {
        require(code!=Z_MEM_ERROR,"decode_limit_exceeded","could not allocate PSD ZIP decoder");
        require(code==Z_OK || code==Z_STREAM_END,"decode_failed","invalid or truncated PSD ZIP stream");
    }

    void step(Source& source) {
        if(!stream_.avail_in && next_offset_<source.size()) {
            const auto chunk=static_cast<unsigned>(std::min<Offset>(input_.size(),source.size()-next_offset_));
            source.read(next_offset_,input_.data(),chunk);
            next_offset_+=chunk;
            stream_.next_in=input_.data();
            stream_.avail_in=chunk;
        }
        const auto in=stream_.avail_in,out=stream_.avail_out;
        const int code=inflate(&stream_,Z_NO_FLUSH);
        check(code);
        ended_=code==Z_STREAM_END;
        require(ended_ || in!=stream_.avail_in || out!=stream_.avail_out,
            "decode_failed","incomplete PSD ZIP stream");
    }

    z_stream stream_{};
    std::array<Byte,zip_chunk> input_;
    Offset next_offset_=0;
    bool active_=false,ended_=false;
};

template<class Sample>
Sample remove_background(Sample composite,Sample coverage,float background) {
    // Match OIIO's native integer straight-alpha quantization for a valid matte.
    // Clamp inconsistent matte samples before converting to the unsigned type.
    constexpr float maximum=std::numeric_limits<Sample>::max();
    constexpr float scale=1.0f/maximum;
    const float alpha=coverage*scale;
    if(alpha==0.0f)return 0;
    const float straight=(composite-((1.0f-alpha)*background)/scale)/alpha;
    return static_cast<Sample>(std::clamp(straight,0.0f,maximum));
}

class PsdRows final : public NativeRowSource {
public:
    PsdRows(const std::string& path,const Layout& layout)
        :layout_(layout),source_(path,layout.size,"decode_failed"),
         plane_row_(layout.compression>=zip?std::max<std::size_t>(zip_chunk,layout.row_bytes()):layout.row_bytes()),
         zip_memory_{0,layout.channels*inflate_bytes} {
        require(source_.opened(),"decode_failed","could not reopen PSD source");
        if(layout_.compression==rle)rle_=std::make_unique<RleRows>(source_,layout_);
        if(layout_.compression>=zip) {
            zip_=std::make_unique<Inflater[]>(layout_.channels);
            zip_[0].start(layout_.payload,zip_memory_);
            for(unsigned plane=1;plane<layout_.channels;++plane) {
                zip_[plane].resume(zip_[plane-1]);
                zip_[plane].skip(source_,plane_row_.data(),unsigned(plane_row_.size()),layout_.plane_bytes());
            }
        }
    }

    void read(int first,int last,void* destination) override {
        require(destination && first>=0 && first<last && unsigned(first)==next_row_ &&
            unsigned(last)<=layout_.height,"decode_failed","PSD rows must be read once in order");
        auto* out=static_cast<Byte*>(destination);
        while(next_row_<unsigned(last)) {
            if(layout_.sample_bytes==1)read_row<std::uint8_t>(out);
            else read_row<std::uint16_t>(out);
            out+=layout_.row_bytes()*layout_.channels;
            ++next_row_;
        }
    }

    void finish() override {
        require(next_row_==layout_.height,"decode_failed","incomplete PSD composite row read");
        if(finished_)return;
        if(zip_) {
            auto& last=zip_[layout_.channels-1];
            last.skip(source_,plane_row_.data(),unsigned(plane_row_.size()),
                layout_.plane_bytes()*(layout_.planes-layout_.channels));
            last.finish(source_);
        }
        if(rle_) {
            for(unsigned plane=layout_.channels;plane<layout_.planes;++plane)
                for(unsigned y=0;y<layout_.height;++y)rle_->read(plane,y,plane_row_.data());
        }
        finished_=true;
    }

private:
    void read_plane(unsigned plane) {
        if(zip_)zip_[plane].read(source_,plane_row_.data(),layout_.row_bytes());
        else if(rle_)rle_->read(plane,next_row_,plane_row_.data());
        else source_.read(layout_.payload+plane*layout_.plane_bytes()+Offset(next_row_)*layout_.row_bytes(),
            plane_row_.data(),layout_.row_bytes());
    }

    template<class Sample>
    void read_row(Byte* out) {
        // Decode alpha first so RGB can be corrected directly in the caller's
        // row without allocating a second interleaved or planar buffer.
        for(unsigned i=layout_.channels;i>0;--i) {
            const unsigned channel=i-1;
            read_plane(channel);
            Sample previous=0;
            for(unsigned x=0;x<layout_.width;++x) {
                const auto* bytes=plane_row_.data()+x*sizeof(Sample);
                Sample value;
                if constexpr(sizeof(Sample)==1)value=*bytes;
                else value=static_cast<Sample>(be16(bytes));
                if(layout_.compression==predicted_zip) {
                    value=static_cast<Sample>(value+previous);
                    previous=value;
                }
                auto* pixel=out+x*layout_.channels*sizeof(Sample);
                if(layout_.channels==4 && channel<3) {
                    Sample alpha;
                    std::memcpy(&alpha,pixel+3*sizeof(Sample),sizeof(Sample));
                    value=remove_background(value,alpha,layout_.background[channel]);
                }
                std::memcpy(pixel+channel*sizeof(Sample),&value,sizeof(Sample));
            }
        }
    }

    Layout layout_;
    Source source_;
    std::vector<Byte> plane_row_;
    InflateMemory zip_memory_;
    std::unique_ptr<Inflater[]> zip_;
    std::unique_ptr<RleRows> rle_;
    unsigned next_row_=0;
    bool finished_=false;
};
}

std::optional<PsdProbe> probe_psd_rows(const std::string& input,Offset source_size,
    Offset decode_budget,std::size_t icc_limit,bool load_icc) {
    Source source(input,source_size,"open_failed");
    if(!source.opened() || source_size<4)return std::nullopt;
    Cursor file(source,{0,source_size});
    const auto signature=file.read<4>();
    if(std::memcmp(signature.data(),"8BPS",4)!=0)return std::nullopt;
    if(be16(file.read<2>().data())!=1)return std::nullopt;
    const auto header=file.read<20>();
    const auto depth=be16(header.data()+16),mode=be16(header.data()+18);
    if(mode!=3 || (depth!=8 && depth!=16))return std::nullopt;
    require(std::all_of(header.begin(),header.begin()+6,[](Byte value){return value==0;}),
        "open_failed","invalid PSD reserved header fields");

    Layout layout;
    layout.size=source_size;
    layout.height=be32(header.data()+8);
    layout.width=be32(header.data()+12);
    layout.planes=be16(header.data()+6);
    layout.sample_bytes=depth/8;
    require(layout.width && layout.height && layout.width<=max_dimension && layout.height<=max_dimension,
        "invalid_dimensions","invalid PSD dimensions");
    require(layout.planes>=3 && layout.planes<=max_channels,"open_failed","invalid PSD RGB channel count");
    require(!file.sized_section().size,"open_failed","unexpected PSD RGB color mode data");
    const auto resource_section=file.sized_section();
    const auto layer_section=file.sized_section();
    const auto metadata=resources(source,resource_section);
    require(!metadata.missing_composite && file.remaining()>=2,
        "decode_failed","PSD does not contain a valid merged image");
    layout.compression=be16(file.read<2>().data());
    if(layout.compression>predicted_zip)return std::nullopt;
    layout.payload=source_size-file.remaining();
    require(file.remaining()>0,"decode_failed","PSD merged image is missing its pixel data");
    layout.channels=merged_alpha(source,layer_section,layout.sample_bytes)?4:3;
    require(layout.channels<=layout.planes,"open_failed","PSD merged transparency channel is missing");
    switch(layout.compression) {
    case raw:
        require(file.remaining()==layout.plane_bytes()*layout.planes,
            "decode_failed","PSD raw composite length does not match its layout");
        break;
    case rle:
        require(file.remaining()>layout.rle_table_bytes(),"decode_failed","truncated PSD PackBits table or payload");
        break;
    }
    if(layout.channels==4 && metadata.background.offset) {
        Cursor background(source,metadata.background);
        const auto bytes=background.read<5>();
        // Preserve the locked PSD plugin's interpretation of resource 1010,
        // with white as the default when the resource is absent.
        for(unsigned c=0;c<3;++c)layout.background[c]=bytes[4-c]*(1.0f/255.0f);
    }

    // Include the planning copy and the ImageSpec copy used for selection/readback.
    const Offset icc_bytes=load_icc?metadata.icc.size:0;
    if(icc_bytes>std::min(icc_limit,metadata_limit))throw CmsFailure("PSD ICC resource exceeds profile limit");
    const auto retained=checked_bytes_add(2*icc_bytes,checked_bytes_add(input.size(),4096));
    // Include stream buffering, the reader object and temporary native path encoding.
    Offset codec=checked_bytes_add(64*1024+sizeof(PsdRows)+layout.row_bytes(),
        checked_bytes_mul(2,input.size()));
    if(layout.compression==rle)codec+=sizeof(RleRows)+65535+2*rle_entries;
    if(layout.compression>=zip)codec+=zip_chunk+layout.channels*(sizeof(Inflater)+inflate_bytes);
    admit(checked_bytes_add(retained,codec),decode_budget,"PSD metadata and row decoder exceed decode budget");

    PsdProbe result;
    result.compression=layout.compression;
    result.has_icc=metadata.icc.offset!=0;
    auto& spec=result.spec;
    spec=OIIO::ImageSpec(int(layout.width),int(layout.height),int(layout.channels),
        depth==8?OIIO::TypeDesc::UINT8:OIIO::TypeDesc::UINT16);
    if(layout.channels==4)spec.attribute("oiio:UnassociatedAlpha",1);
    add_orientation(source,metadata,spec);
    if(icc_bytes) {
        std::vector<Byte> profile(static_cast<std::size_t>(icc_bytes));
        source.read(metadata.icc.offset,profile.data(),profile.size());
        spec.attribute("ICCProfile",OIIO::TypeDesc(OIIO::TypeDesc::UINT8,int(icc_bytes)),profile.data());
    }
    result.read={layout.channels*layout.sample_bytes,retained,codec,
        [input,layout]{return std::make_unique<PsdRows>(input,layout);}};
    return result;
}
}
