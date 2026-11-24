#include "tiff_rows.h"
#include "files.h"
#include <tiffio.h>
#include <cstring>
#include <fstream>

namespace thumbgen::detail {
namespace {
using Tiff=std::unique_ptr<TIFF,decltype(&TIFFClose)>;
using OpenOptions=std::unique_ptr<TIFFOpenOptions,decltype(&TIFFOpenOptionsFree)>;

// The stock error printer would write to stderr in a worker. Record budget
// denials and leave malformed-directory reporting to the original reader.
int tiff_message(TIFF*,void* context,const char*,const char* format,va_list) {
    if(context && format && std::strstr(format,"defined in open options"))
        *static_cast<bool*>(context)=true;
    return 1;
}

Tiff open_tiff(const std::filesystem::path& path,std::uint64_t budget,const char* mode,bool* denied=nullptr) {
    OpenOptions options(TIFFOpenOptionsAlloc(),TIFFOpenOptionsFree);
    if(!options)throw std::bad_alloc();
    const auto limit=std::min<std::uint64_t>(budget,std::numeric_limits<tmsize_t>::max());
    TIFFOpenOptionsSetMaxSingleMemAlloc(options.get(),tmsize_t(limit));
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(options.get(),tmsize_t(limit));
    TIFFOpenOptionsSetErrorHandlerExtR(options.get(),tiff_message,denied);
    TIFFOpenOptionsSetWarningHandlerExtR(options.get(),tiff_message,denied);
#ifdef _WIN32
    return Tiff(TIFFOpenWExt(path.c_str(),mode,options.get()),TIFFClose);
#else
    return Tiff(TIFFOpenExt(path.c_str(),mode,options.get()),TIFFClose);
#endif
}

std::uint64_t bytes_mul(std::uint64_t a,std::uint64_t b) {
    if(a && b>std::numeric_limits<std::uint64_t>::max()/a)
        throw FileFailure("decode_limit_exceeded","TIFF chunk size overflow");
    return a*b;
}

std::uint64_t ceil_div(std::uint64_t value,std::uint64_t unit) {
    return value/unit+(value%unit!=0);
}

bool bounded_compression(unsigned compression) {
    switch(compression) {
        case COMPRESSION_NONE: case COMPRESSION_ADOBE_DEFLATE: case COMPRESSION_DEFLATE:
        case COMPRESSION_LZW: case COMPRESSION_PACKBITS: return true;
        default: return false;
    }
}

std::uint64_t spec_metadata_bytes(const OIIO::ImageSpec& spec) {
    std::uint64_t bytes=0;
    for(const auto& attr:spec.extra_attribs) {
        bytes=checked_bytes_add(bytes,attr.name().size()+sizeof(OIIO::ParamValue));
        if(attr.datasize()>0)bytes=checked_bytes_add(bytes,std::uint64_t(attr.datasize()));
        if(attr.type().basetype==OIIO::TypeDesc::STRING)
            for(auto value:attr.as_cspan<OIIO::ustring>())
                bytes=checked_bytes_add(bytes,value.size()+1);
    }
    return bytes_mul(bytes,4);
}

struct TiffLayout {
    std::uint32_t width=0,height=0,depth=1,chunk_width=0,chunk_height=0,chunk_depth=1;
    std::uint16_t channels=0,bits=0,format=0,photometric=0,planar=0,compression=0;
    std::uint64_t chunk_bytes=0,across=0,slab_bytes=0,row_bytes=0;
    std::uint32_t chunks=0;
    bool tiled=false;
};

TiffLayout tiff_layout(TIFF* tif,const OIIO::ImageSpec& spec,std::uint64_t budget) {
    TiffLayout layout;
    TIFFGetField(tif,TIFFTAG_IMAGEWIDTH,&layout.width);
    TIFFGetField(tif,TIFFTAG_IMAGELENGTH,&layout.height);
    TIFFGetFieldDefaulted(tif,TIFFTAG_IMAGEDEPTH,&layout.depth);
    TIFFGetFieldDefaulted(tif,TIFFTAG_SAMPLESPERPIXEL,&layout.channels);
    TIFFGetFieldDefaulted(tif,TIFFTAG_BITSPERSAMPLE,&layout.bits);
    TIFFGetFieldDefaulted(tif,TIFFTAG_SAMPLEFORMAT,&layout.format);
    TIFFGetFieldDefaulted(tif,TIFFTAG_PHOTOMETRIC,&layout.photometric);
    TIFFGetFieldDefaulted(tif,TIFFTAG_PLANARCONFIG,&layout.planar);
    TIFFGetFieldDefaulted(tif,TIFFTAG_COMPRESSION,&layout.compression);
    layout.tiled=TIFFIsTiled(tif)!=0;
    if(layout.width!=std::uint64_t(spec.width) || layout.height!=std::uint64_t(spec.height) ||
       layout.tiled!=(spec.tile_width>0) || !layout.width || !layout.height || !layout.depth)
        throw FileFailure("decode_failed","TIFF directory differs from selected layout");
    if(layout.tiled) {
        TIFFGetField(tif,TIFFTAG_TILEWIDTH,&layout.chunk_width);
        TIFFGetField(tif,TIFFTAG_TILELENGTH,&layout.chunk_height);
        TIFFGetFieldDefaulted(tif,TIFFTAG_TILEDEPTH,&layout.chunk_depth);
    } else {
        layout.chunk_width=layout.width;
        TIFFGetFieldDefaulted(tif,TIFFTAG_ROWSPERSTRIP,&layout.chunk_height);
        // UINT32_MAX is the defined default: one strip for the image.
        layout.chunk_height=std::min(layout.chunk_height,layout.height);
    }
    if(!layout.chunk_width || !layout.chunk_height || !layout.chunk_depth ||
       !layout.channels || !layout.bits)
        throw FileFailure("decode_failed","invalid TIFF chunk geometry");

    const auto pixels=bytes_mul(bytes_mul(layout.chunk_width,layout.chunk_height),layout.chunk_depth);
    const auto container_bytes=ceil_div(bytes_mul(pixels,layout.bits),8);
    // Even a single sample plane must fit; check independently of libtiff's
    // size return so a wrapped size cannot be used to admit a huge tile.
    if(container_bytes>budget || container_bytes>std::uint64_t(std::numeric_limits<tmsize_t>::max()))
        throw FileFailure("decode_limit_exceeded","TIFF strip or tile exceeds decode budget");

    layout.chunk_bytes=layout.tiled?TIFFTileSize64(tif):TIFFStripSize64(tif);
    layout.across=ceil_div(layout.width,layout.chunk_width);
    const auto planes=layout.planar==PLANARCONFIG_SEPARATE?layout.channels:1;
    const auto count=bytes_mul(bytes_mul(layout.across,ceil_div(layout.height,layout.chunk_height)),
                               bytes_mul(ceil_div(layout.depth,layout.chunk_depth),planes));
    layout.chunks=layout.tiled?TIFFNumberOfTiles(tif):TIFFNumberOfStrips(tif);
    if(!layout.chunk_bytes || count!=layout.chunks)
        throw FileFailure("decode_failed","inconsistent TIFF strip or tile layout");
    if(layout.chunk_bytes>budget || layout.chunk_bytes>std::uint64_t(std::numeric_limits<int>::max()) ||
       count>budget/32)
        throw FileFailure("decode_limit_exceeded","TIFF chunk or offset table exceeds decode budget");
    layout.row_bytes=bytes_mul(layout.width,spec.pixel_bytes(true));
    layout.slab_bytes=bytes_mul(layout.row_bytes,std::min(layout.height,layout.chunk_height));
    return layout;
}

struct StrileBytes {
    std::uint64_t largest=0;
    bool sparse=false;
};

StrileBytes strile_bytes(TIFF* tif,std::uint32_t chunks,std::uint64_t size) {
    StrileBytes result;
    for(std::uint32_t i=0;i<chunks;++i) {
        int count_error=0,offset_error=0;
        const auto count=TIFFGetStrileByteCountWithErr(tif,i,&count_error);
        const auto offset=TIFFGetStrileOffsetWithErr(tif,i,&offset_error);
        if(count_error || offset_error || offset>size || count>size-offset)
            throw FileFailure("decode_failed","invalid TIFF strip or tile extent");
        result.sparse|=!offset || !count;
        result.largest=std::max(result.largest,count);
    }
    return result;
}

bool matching_samples(const TiffLayout& l,const OIIO::ImageSpec& spec,TIFF* tif) {
    const int colours=l.photometric==PHOTOMETRIC_RGB?3:1;
    if(l.photometric!=PHOTOMETRIC_RGB && l.photometric!=PHOTOMETRIC_MINISBLACK &&
       l.photometric!=PHOTOMETRIC_MINISWHITE)return false;
    if(l.channels!=spec.nchannels || l.channels<colours || l.channels>colours+1)return false;
    // MINISWHITE plus alpha is ambiguous in the OIIO paths, which can
    // invert every component. Keep the original read path for that pairing.
    if(l.photometric==PHOTOMETRIC_MINISWHITE && l.channels!=1)return false;
    if(l.channels==colours)return spec.alpha_channel<0;
    std::uint16_t count=0;
    std::uint16_t* extra=nullptr;
    if(!TIFFGetField(tif,TIFFTAG_EXTRASAMPLES,&count,&extra) || count!=1 || !extra ||
       spec.alpha_channel!=colours)return false;
    return spec.get_int_attribute("oiio:UnassociatedAlpha",0)?*extra==EXTRASAMPLE_UNASSALPHA:
        *extra==EXTRASAMPLE_ASSOCALPHA;
}

class TiffRows final : public NativeRowSource {
public:
    TiffRows(Tiff tif,TiffLayout layout)
        :tif_(std::move(tif)),layout_(layout),slab_(std::size_t(layout.slab_bytes)),
        tile_(layout.tiled?std::size_t(layout.chunk_bytes):0) {}

    void read(int first,int last,void* destination) override {
        if(first<0 || std::uint32_t(first)!=next_ || last<=first || std::uint32_t(last)>layout_.height)
            throw FileFailure("decode_failed","TIFF rows must be read once in order");
        auto* output=static_cast<unsigned char*>(destination);
        while(next_<std::uint32_t(last)) {
            if(next_==group_end_)load_group();
            const auto rows=std::min(group_end_,std::uint32_t(last))-next_;
            const auto bytes=rows*layout_.row_bytes;
            std::memcpy(output,slab_.data()+(next_-group_begin_)*layout_.row_bytes,std::size_t(bytes));
            next_+=rows;
            output+=bytes;
        }
    }

    void finish() override {
        if(next_!=layout_.height)throw FileFailure("decode_failed","incomplete TIFF image");
        tif_.reset();
    }

private:
    void load_group() {
        group_begin_=next_;
        const auto rows=std::min(layout_.chunk_height,layout_.height-next_);
        group_end_=group_begin_+rows;
        const auto group=group_begin_/layout_.chunk_height;
        const auto row_bytes=layout_.row_bytes;
        const auto slab_bytes=rows*row_bytes;
        if(!layout_.tiled) {
            const auto bytes=tmsize_t(slab_bytes);
            if(TIFFReadEncodedStrip(tif_.get(),group,slab_.data(),bytes)!=bytes)
                throw FileFailure("decode_failed","could not decode TIFF strip");
        } else {
            const auto pixel_bytes=row_bytes/layout_.width;
            const auto tile_row=layout_.chunk_width*pixel_bytes;
            for(std::uint64_t col=0;col<layout_.across;++col) {
                const auto index=std::uint32_t(group*layout_.across+col);
                const auto bytes=tmsize_t(layout_.chunk_bytes);
                if(TIFFReadEncodedTile(tif_.get(),index,tile_.data(),bytes)!=bytes)
                    throw FileFailure("decode_failed","could not decode TIFF tile");
                const auto x=col*layout_.chunk_width;
                const auto valid_bytes=std::min<std::uint64_t>(layout_.width-x,layout_.chunk_width)*pixel_bytes;
                for(std::uint32_t y=0;y<rows;++y)
                    std::memcpy(slab_.data()+y*row_bytes+x*pixel_bytes,tile_.data()+y*tile_row,std::size_t(valid_bytes));
            }
        }
        // Match the existing TIFF plugin's native UINT8 MINISWHITE read.
        if(layout_.photometric==PHOTOMETRIC_MINISWHITE && layout_.bits==8)
            for(std::uint64_t i=0;i<slab_bytes;++i)slab_[i]=255-slab_[i];
    }

    Tiff tif_;
    TiffLayout layout_;
    std::vector<unsigned char> slab_,tile_;
    std::uint32_t next_=0,group_begin_=0,group_end_=0;
};
}

bool preflight_tiff_header(const std::filesystem::path& path,std::uint64_t budget) {
    std::ifstream file(path,std::ios::binary);
    unsigned char h[4]{};
    if(!file.read(reinterpret_cast<char*>(h),sizeof(h)))return false;
    const bool little=h[0]=='I' && h[1]=='I' && (h[2]==42 || h[2]==43) && h[3]==0;
    const bool big=h[0]=='M' && h[1]=='M' && h[2]==0 && (h[3]==42 || h[3]==43);
    if(!little && !big)return false;
    file.close();

    bool denied=false;
    // Mirror the OIIO reader's eager table loading, default strip chopping,
    // EXIF/GPS loads and directory traversal, without reading any pixels.
    auto input=open_tiff(path,budget,"rm",&denied);
    if(input) {
        do {
            const auto page=TIFFCurrentDirectory(input.get());
            for(const auto tag:{TIFFTAG_EXIFIFD,TIFFTAG_GPSIFD}) {
                toff_t offset=0;
                if(TIFFGetField(input.get(),tag,&offset)) {
                    TIFFReadEXIFDirectory(input.get(),offset);
                    if(denied || !TIFFSetDirectory(input.get(),page))break;
                }
            }
        } while(!denied && !TIFFLastDirectory(input.get()) && TIFFReadDirectory(input.get()));
    }
    if(denied)throw FileFailure("decode_limit_exceeded","TIFF directory metadata exceeds decode budget");
    return true;
}

TiffRowPlan plan_tiff_rows(const std::filesystem::path& path,const OIIO::ImageSpec& spec,
    int part,std::uint64_t budget) {
    // Prevent mmap and use on-disk strip geometry for the native reader.
    auto tif=open_tiff(path,budget,"rmch");
    if(!tif || part<0 || !TIFFSetDirectory(tif.get(),tdir_t(part)))
        throw FileFailure("decode_failed","could not inspect selected TIFF directory within decode budget");

    const auto layout=tiff_layout(tif.get(),spec,budget);
    const auto extents=strile_bytes(tif.get(),layout.chunks,std::filesystem::file_size(path));
    if(extents.largest>budget)
        throw FileFailure("decode_limit_exceeded","compressed TIFF strip or tile exceeds decode budget");
    const auto raw=checked_bytes_add(bytes_mul(extents.largest,2),64*1024);
    const auto tables=bytes_mul(layout.chunks,32);
    // The native codecs are serial. Cover the compressed buffer, replacement
    // during growth, directory arrays, libtiff state and inflate/LZW tables.
    const bool known_codec=bounded_compression(layout.compression);
    const auto metadata=checked_bytes_add(tables,spec_metadata_bytes(spec));
    const auto codec=checked_bytes_add(checked_bytes_add(raw,metadata),known_codec?512*1024:8*bounded_mib);
    const auto group=layout.tiled?bytes_mul(layout.chunk_bytes,layout.across):layout.slab_bytes;
    if(codec>budget)throw FileFailure("decode_limit_exceeded","TIFF decoder scratch exceeds decode budget");

    TiffRowPlan result;
    // The complete OIIO reader can retain several native/padded tile copies,
    // or batches of separate planes/raw ZIP strips. Include that working
    // storage and the optional whole-image RGBA conversion for legacy codecs.
    const auto native_full=bytes_mul(layout.row_bytes,layout.height);
    const auto chunk_guard=layout.tiled?bytes_mul(std::max(group,
        bytes_mul(bytes_mul(bytes_mul(layout.chunk_width,layout.chunk_height),layout.chunk_depth),
            std::max<std::uint64_t>(spec.pixel_bytes(true),4ULL*spec.nchannels))),4):
        checked_bytes_add(bytes_mul(layout.slab_bytes,4),bytes_mul(std::min(native_full,64*bounded_mib),2));
    const auto photo=layout.photometric;
    const bool rgba_interface=photo==PHOTOMETRIC_YCBCR || photo==PHOTOMETRIC_CIELAB ||
        photo==PHOTOMETRIC_ICCLAB || photo==PHOTOMETRIC_ITULAB ||
        photo==PHOTOMETRIC_LOGL || photo==PHOTOMETRIC_LOGLUV ||
        ((layout.compression==COMPRESSION_JPEG || layout.compression==COMPRESSION_OJPEG) && spec.nchannels!=3);
    const auto legacy=rgba_interface?bytes_mul(bytes_mul(layout.width,layout.height),4):0;
    result.fallback_codec_bytes=checked_bytes_add(checked_bytes_add(codec,chunk_guard),legacy);

    if(!known_codec || layout.planar!=PLANARCONFIG_CONTIG || layout.format!=SAMPLEFORMAT_UINT ||
       (layout.bits!=8 && layout.bits!=16) || layout.depth!=1 || layout.chunk_depth!=1 ||
       layout.width>65536 || layout.height>65536 || spec.depth!=1 || !spec.channelformats.empty() ||
       spec.format!=(layout.bits==16?OIIO::TypeDesc::UINT16:OIIO::TypeDesc::UINT8) ||
       extents.sparse || !matching_samples(layout,spec,tif.get()))return result;

    const auto pixel=unsigned(layout.channels*(layout.bits/8));
    const auto packed_chunk=bytes_mul(bytes_mul(layout.chunk_width,layout.chunk_height),pixel);
    if(layout.chunk_bytes!=packed_chunk || layout.row_bytes!=std::uint64_t(layout.width)*pixel)
        throw FileFailure("decode_failed","TIFF native chunk byte size differs from selected layout");
    const auto retained=checked_bytes_add(layout.slab_bytes,layout.tiled?layout.chunk_bytes:0);
    if(checked_bytes_add(retained,codec)>budget)
        throw FileFailure("decode_limit_exceeded","TIFF native strip or tile group exceeds decode budget");

    result.read={pixel,retained,codec,[path,part,layout,codec] {
        // Apply the admitted codec reservation to the decode handle itself.
        // Reserve part of it for zlib/libdeflate allocations outside libtiff.
        auto decoder=open_tiff(path,codec-256*1024,"rmch");
        if(!decoder || !TIFFSetDirectory(decoder.get(),tdir_t(part)))
            throw FileFailure("decode_failed","could not open admitted TIFF directory");
        return std::make_unique<TiffRows>(std::move(decoder),layout);
    }};
    return result;
}
}
