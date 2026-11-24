#include "png_rows.h"
#include "files.h"
#include <zlib.h>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>

namespace thumbgen::detail {
namespace {
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
}

struct PngHeader {
    std::uint32_t width,height;
    unsigned bits,type,interlace,channels;
};

std::optional<PngHeader> png_header(const std::filesystem::path& path) {
    std::array<unsigned char,33> h{};
    const unsigned char prefix[]{137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R'};
    std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(h.data()),h.size()) ||
       !std::equal(std::begin(prefix),std::end(prefix),h.begin()) ||
       h[26]!=0 || h[27]!=0 || h[28]>1 || crc32(0,h.data()+12,17)!=be32(h.data()+29))return {};

    const auto width=be32(h.data()+16),height=be32(h.data()+20);
    if(!width || !height || width>0x7fffffff || height>0x7fffffff)return {};
    const unsigned bits=h[24],type=h[25];
    const bool whole_sample=bits==8 || bits==16;
    unsigned channels=0;
    switch(type) {
        case 0: if(whole_sample || bits==1 || bits==2 || bits==4)channels=1;break;
        case 2: if(whole_sample)channels=3;break;
        case 3: if(bits==1 || bits==2 || bits==4 || bits==8)channels=3;break;
        case 4: if(whole_sample)channels=2;break;
        case 6: if(whole_sample)channels=4;break;
    }
    if(!channels)return {};
    return PngHeader{width,height,bits,type,h[28],channels};
}

std::uint64_t frame_bytes(std::uint64_t row,std::uint64_t height,std::uint64_t budget) {
    if(row>budget/height)
        throw FileFailure("decode_limit_exceeded","Adam7 PNG native frame exceeds decode budget");
    return row*height;
}

std::uint64_t metadata_bytes(const std::filesystem::path& path,bool include_tail,std::uint64_t budget) {
    std::ifstream file(path,std::ios::binary);
    file.seekg(33);
    std::uint64_t bytes=0;
    std::array<unsigned char,8> h{};
    while(file.read(reinterpret_cast<char*>(h.data()),h.size())) {
        const auto length=be32(h.data());
        const auto is=[&](const char* name) {return std::memcmp(h.data()+4,name,4)==0;};
        if(is("IEND"))break;
        if(is("IDAT")) {
            if(!include_tail)break;
        } else {
            bool deflated=is("iCCP") || is("zTXt");
            if(is("iTXt")) {
                // The optional compression flag follows the NUL-terminated
                // keyword, which PNG restricts to 79 bytes.
                std::array<unsigned char,81> prefix{};
                const auto count=std::min<std::size_t>(length,prefix.size());
                const auto start=file.tellg();
                if(!file.read(reinterpret_cast<char*>(prefix.data()),std::streamsize(count)))break;
                const auto end=prefix.begin()+count;
                const auto nul=std::find(prefix.begin(),end,0);
                deflated=end-nul>=2 && nul[1]!=0;
                file.seekg(start);
            }
            // A DEFLATE length/distance pair consumes at least two bits and
            // produces at most 258 bytes. Also cover retained raw metadata,
            // transient inflate copies and copies in the image spec.
            const auto payload=deflated?std::uint64_t(length)*1033:length;
            bytes=checked_bytes_add(bytes,checked_bytes_mul(payload+128,8));
            if(bytes>budget)throw FileFailure("decode_limit_exceeded","PNG metadata exceeds decode budget");
        }
        file.seekg(std::streamoff(length)+4,std::ios::cur);
    }
    return bytes;
}

class PngRows final : public NativeRowSource {
public:
    PngRows(OIIO::ImageInput& input,int y,int height,int z,std::size_t row_bytes)
        :input_(input),y_(y),height_(height),z_(z),row_bytes_(row_bytes) {}

    void read(int first,int last,void* destination) override {
        if(first!=next_ || last<=first || last>height_)
            throw FileFailure("decode_failed","PNG rows must be read once in order");
        auto* output=static_cast<unsigned char*>(destination);
        for(;next_<last;++next_,output+=row_bytes_) {
            if(!input_.read_native_scanline(0,0,y_+next_,z_,output))
                throw FileFailure("decode_failed",input_.geterror());
        }
    }

    void finish() override {
        if(next_!=height_)throw FileFailure("decode_failed","incomplete PNG image");
        input_.close();
    }

private:
    OIIO::ImageInput& input_;
    int y_,height_,z_,next_=0;
    std::size_t row_bytes_;
};
}

bool preflight_png_header(const std::filesystem::path& path,std::uint64_t budget) {
    const auto header=png_header(path);
    if(!header)return false;
    const auto row=std::uint64_t(header->width)*header->channels*(header->bits==16?2:1);
    if(header->interlace)frame_bytes(row,header->height,budget);
    const auto info=metadata_bytes(path,header->interlace!=0,budget);
    if(checked_bytes_add(checked_bytes_mul(row,8),info)>budget)
        throw FileFailure("decode_limit_exceeded","PNG header and rows exceed decode budget");
    return true;
}

PngRowPlan plan_png_rows(const std::filesystem::path& path,OIIO::ImageInput& input,
    const OIIO::ImageSpec& spec,std::uint64_t budget) {
    const auto header=png_header(path);
    if(!header || header->width!=std::uint64_t(spec.width) || header->height!=std::uint64_t(spec.height))
        throw FileFailure("open_failed","inconsistent PNG header");

    const auto pixel=spec.pixel_bytes(true);
    const auto row=checked_bytes_mul(header->width,pixel);
    // libpng keeps filter/previous/expanded rows, its zlib state and header
    // metadata. Include the libpng iCCP allocation even if CMS is bypassed.
    auto codec=checked_bytes_add(checked_bytes_mul(row,8),512*1024);
    codec=checked_bytes_add(codec,metadata_bytes(path,header->interlace!=0,budget));

    std::uint64_t frame=0;
    if(header->interlace) {
        frame=frame_bytes(row,header->height,budget);
        // OIIO's Adam7 read constructs one row pointer per scanline.
        codec=checked_bytes_add(codec,checked_bytes_mul(header->height,sizeof(void*)));
    }
    PngRowPlan result;
    result.fallback_codec_bytes=checked_bytes_add(codec,frame);
    result.retains_frame=frame!=0;
    if(result.fallback_codec_bytes>budget)
        throw FileFailure("decode_limit_exceeded","PNG frame and decoder scratch exceed decode budget");

    const unsigned bytes=header->bits==16?2:1;
    const bool have_alpha=spec.nchannels==int(header->channels)+1 &&
        (header->type==0 || header->type==2) && spec.alpha_channel==spec.nchannels-1;
    if(header->type==3 || (header->bits!=8 && header->bits!=16) ||
       spec.depth!=1 || spec.tile_width || !spec.channelformats.empty() ||
       (spec.nchannels!=int(header->channels) && !have_alpha) ||
       spec.format!=(bytes==2?OIIO::TypeDesc::UINT16:OIIO::TypeDesc::UINT8) ||
       header->width>65536 || header->height>65536 ||
       std::int64_t(spec.y)+spec.height-1>std::numeric_limits<int>::max())return result;

    result.read={unsigned(pixel),frame,codec,[&input,y=spec.y,height=spec.height,z=spec.z,row] {
        return std::make_unique<PngRows>(input,y,height,z,std::size_t(row));
    }};
    return result;
}
}
