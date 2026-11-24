#include "other_rows_internal.h"
#include "cms_input.h"
#include <OpenImageIO/tiffutils.h>

namespace thumbgen::detail::other_rows {
namespace {
constexpr std::uint64_t chunk_limit=65536;
struct Chunk {
    std::uint64_t offset=0;
    std::uint32_t size=0;
};
void record(Chunk& chunk,std::uint64_t offset,std::uint32_t size) {
    require(chunk.offset==0,"duplicate WebP chunk");
    chunk={offset,size};
}

std::uint64_t lossless_scratch(std::uint64_t width,std::uint64_t height) {
    // VP8L maintains an ARGB decode cache and subsampled transform maps.
    // Entropy groups are indexed on a grid with tiles of at least 4x4;
    // account for group mapping and Huffman tables even for small images.
    const auto groups=std::min<std::uint64_t>(65536,((width+3)/4)*((height+3)/4));
    return 8*width*height+65536*groups;
}
}

OtherRowsProbe webp(Source& source,std::size_t icc_limit) {
    // OIIO allocates the entire physical file, including trailing bytes.
    reserve(source.size,source.budget,"WebP compressed source exceeds decode budget");
    std::array<Byte,10> header{};
    source.read(4,header.data(),4,"truncated WebP container");
    const auto end=8+std::uint64_t(le32(header.data()));
    source.extent(0,end,"truncated WebP container");
    Chunk extended,vp8,vp8l,alpha,icc,exif,animation;
    std::uint64_t count=0,metadata_bytes=0;
    unsigned flags=0,width=0,height=0;
    bool frames=false;
    for(std::uint64_t position=12;position<end;) {
        reserve(++count,chunk_limit,"WebP container has too many chunks");
        require(end-position>=8,"truncated WebP chunk header");
        source.read(position,header.data(),8,"truncated WebP chunk header");
        const auto size=le32(header.data()+4);
        const auto payload=position+8,padded=std::uint64_t(size)+(size&1);
        require(padded<=end-payload,"truncated WebP chunk payload");
        if(is(header.data(),"VP8X")) {
            require(position==12 && size==10,"invalid WebP extended header");
            record(extended,payload,size);
            source.read(payload,header.data(),10,"truncated WebP extended header");
            flags=header[0];
            require(!(flags&~0x3e) && !le24(header.data()+1),"invalid WebP extended flags");
            width=le24(header.data()+4)+1;
            height=le24(header.data()+7)+1;
            dimensions(width,height,16383,"WebP canvas dimensions exceed supported limits");
        } else if(is(header.data(),"VP8 "))record(vp8,payload,size);
        else if(is(header.data(),"VP8L"))record(vp8l,payload,size);
        else if(is(header.data(),"ALPH"))record(alpha,payload,size);
        else if(is(header.data(),"ICCP")) {
            if(size>icc_limit)throw CmsFailure("WebP ICC chunk exceeds profile limit");
            record(icc,payload,size);
            metadata_bytes+=size;
        } else if(is(header.data(),"EXIF")) {
            record(exif,payload,size);
            metadata_bytes+=size;
        } else if(is(header.data(),"ANIM"))record(animation,payload,size);
        else if(is(header.data(),"ANMF"))frames=true;
        position=payload+padded;
    }
    const bool animated=(flags&2)!=0;
    require(animated==(animation.offset!=0) && animated==frames,"invalid WebP animation layout");
    require(animated?(!vp8.offset && !vp8l.offset):(bool(vp8.offset)!=bool(vp8l.offset)),
        "invalid WebP image chunk layout");
    bool lossy_alpha=false;
    if(!animated) {
        unsigned image_width=0,image_height=0;
        if(vp8.offset) {
            require(vp8.size>=10,"truncated WebP VP8 header");
            source.read(vp8.offset,header.data(),10,"truncated WebP VP8 header");
            require(!(header[0]&1) && header[3]==0x9d && header[4]==1 && header[5]==0x2a,
                "invalid WebP VP8 key frame");
            image_width=le16(header.data()+6)&0x3fff;
            image_height=le16(header.data()+8)&0x3fff;
        } else {
            require(vp8l.size>=5,"truncated WebP lossless header");
            source.read(vp8l.offset,header.data(),5,"truncated WebP lossless header");
            const auto layout=le32(header.data()+1);
            require(header[0]==0x2f && !(layout>>29),"invalid WebP lossless header");
            image_width=(layout&0x3fff)+1;
            image_height=((layout>>14)&0x3fff)+1;
            if(!extended.offset && ((layout>>28)&1))flags|=0x10;
        }
        require(!extended.offset || (width==image_width && height==image_height),
            "WebP canvas and frame dimensions do not match");
        width=image_width;
        height=image_height;
        if(alpha.offset) {
            require(vp8.offset && alpha.size && extended.offset && (flags&0x10),"invalid WebP alpha chunk");
            source.read(alpha.offset,header.data(),1,"truncated WebP alpha header");
            const auto method=header[0]&3;
            require(method<=1 && !(header[0]&0xc0),"invalid WebP alpha encoding");
            if(method==0)require(alpha.size>std::uint64_t(width)*height,"truncated raw WebP alpha");
            lossy_alpha=method==1; // VP8L-compressed alpha in an otherwise VP8 image.
        }
    }
    require(!vp8.offset || bool(flags&0x10)==bool(alpha.offset),"missing WebP alpha data");
    const auto pixels=dimensions(width,height,16383,"WebP canvas dimensions exceed supported limits");
    const bool has_alpha=(flags&0x10)!=0;
    const auto retained=pixels*(has_alpha?4:3);
    reserve(metadata_bytes,metadata_limit,"WebP metadata exceeds limit");
    // The lossy decoder uses macroblock-row caches with no internal RGB
    // output (DecodeRGB[A]Into). Alpha has up to two native byte planes.
    // Reserve demux nodes separately from payload and copied metadata.
    const auto eager=source.size+retained+metadata_bytes*metadata_copies+count*256;
    const auto codec=eager-retained+plugin_bytes+512*std::uint64_t(width)+
        (has_alpha?2*pixels:0)+(lossy_alpha?lossless_scratch(width,height):0);
    reserve(retained+codec,source.budget,"WebP source, native frame and codec scratch exceed decode budget");
    if(animated || vp8l.offset) {
        const auto scratch=lossless_scratch(width,height)+(animated?4*pixels:0);
        return fallback(source,"webp",pixels,retained+codec+scratch,eager);
    }

    OIIO::ImageSpec spec(int(width),int(height),has_alpha?4:3,OIIO::TypeDesc::UINT8);
    spec.set_colorspace("srgb_rec709_scene");
    if(has_alpha && source.config.get_int_attribute("oiio:UnassociatedAlpha",0))
        spec.attribute("oiio:UnassociatedAlpha",1);
    if((flags&8) && exif.size) {
        std::vector<Byte> bytes(exif.size);
        source.read(exif.offset,bytes.data(),bytes.size(),"truncated WebP EXIF");
        const auto prefix=bytes.size()>=6 && is(bytes.data(),"Exif\0\0",6)?6:0;
        if(bytes.size()>=std::size_t(prefix)+8 &&
           !OIIO::decode_exif(bytes,spec) && OIIO::get_int_attribute("imageinput:strict"))
            throw FileFailure("open_failed","could not decode WebP EXIF");
    }
    if((flags&0x20) && icc.offset) {
        std::vector<Byte> bytes(icc.size);
        if(!bytes.empty())source.read(icc.offset,bytes.data(),bytes.size(),"truncated WebP ICC profile");
        if(!bytes.empty())
            spec.attribute("ICCProfile",OIIO::TypeDesc(OIIO::TypeDesc::UINT8,int(bytes.size())),bytes.data());
        std::string error;
        if(!OIIO::decode_icc_profile(bytes,spec,error) && OIIO::get_int_attribute("imageinput:strict"))
            throw FileFailure("open_failed","could not decode WebP ICC profile: "+error);
    }
    return supported(source,"webp",std::move(spec),retained,codec);
}
}
