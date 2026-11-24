#include "jpeg_rows.h"
#include "cms_input.h"
#include "files.h"
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/tiffutils.h>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>
#include <jerror.h>

namespace thumbgen::detail {
namespace {
constexpr std::uint64_t header_limit=8*bounded_mib;
constexpr std::uint64_t metadata_copies=8;
struct JpegError {
    jpeg_error_mgr base{};
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};
void jpeg_failed(j_common_ptr decoder) {
    auto* error=reinterpret_cast<JpegError*>(decoder->err);
    decoder->err->format_message(decoder,error->message);
    std::longjmp(error->jump,1);
}
void jpeg_warning(j_common_ptr decoder,int level) {
    // An invalid ICC sequence is still visible to automatic colour selection.
    // Its existing unsupported-profile error is distinct from a JPEG failure.
    if(level<0 && decoder->err->msg_code!=JWRN_BOGUS_ICC)jpeg_failed(decoder);
}
using File=std::unique_ptr<FILE,decltype(&std::fclose)>;
struct Decoder {
    File file;
    jpeg_decompress_struct jpeg{};
    JpegError error;
    bool started=false;
    explicit Decoder(File source):file(std::move(source)) {
        jpeg.err=jpeg_std_error(&error.base);
        error.base.error_exit=jpeg_failed;
        error.base.emit_message=jpeg_warning;
    }
    ~Decoder(){jpeg_destroy_decompress(&jpeg);}
    void header() {
        if(setjmp(error.jump))throw FileFailure("open_failed",error.message);
        jpeg_create_decompress(&jpeg);
        jpeg_stdio_src(&jpeg,file.get());
        jpeg_save_markers(&jpeg,JPEG_APP0+1,0xffff);
        jpeg_save_markers(&jpeg,JPEG_APP0+2,0xffff);
        if(jpeg_read_header(&jpeg,TRUE)!=JPEG_HEADER_OK)throw FileFailure("open_failed","incomplete JPEG header");
        // APP segments between later scans must not accumulate during decode.
        jpeg_save_markers(&jpeg,JPEG_APP0+1,0);
        jpeg_save_markers(&jpeg,JPEG_APP0+2,0);
    }
};
// Limit the total header before asking libjpeg to retain any markers. This
// also prevents an arbitrary chain of empty segments from becoming unbounded.
std::uint64_t header_bytes(FILE* file) {
    std::uint64_t bytes=2,marker_overhead=0;
    auto byte=[&]() {
        if(++bytes>header_limit)throw FileFailure("open_failed","JPEG header exceeds metadata limit");
        const int value=std::getc(file);
        if(value==EOF)throw FileFailure("open_failed","truncated JPEG header");
        return value;
    };
    for(;;) {
        if(byte()!=0xff)throw FileFailure("open_failed","invalid JPEG header marker");
        int marker=byte();
        while(marker==0xff)marker=byte();
        if(marker==0 || marker==0xd8 || marker==0xd9 || (marker>=0xd0 && marker<=0xd7))
            throw FileFailure("open_failed","invalid JPEG header marker");
        if(marker==1)continue;
        if((marker>=0xe0 && marker<=0xef) || marker==0xfe)marker_overhead+=128;
        const auto high=byte();
        const int size=(high<<8)+byte()-2;
        if(size<0 || (marker==0xda && size<6))throw FileFailure("open_failed","invalid JPEG segment length");
        if(std::uint64_t(size)+marker_overhead>header_limit-bytes)throw FileFailure("open_failed","JPEG header exceeds metadata limit");
        if(std::fseek(file,size,SEEK_CUR)!=0)throw FileFailure("open_failed","cannot read JPEG header");
        bytes+=size;
        if(marker==0xda)return bytes+marker_overhead;
    }
}
std::uint64_t ceil_div(std::uint64_t value,std::uint64_t unit){return value/unit+(value%unit!=0);}
std::uint64_t decoder_bytes(jpeg_decompress_struct& jpeg,std::uint64_t header) {
    unsigned max_h=1,max_v=1;
    for(int c=0;c<jpeg.num_components;++c) {
        const auto& comp=jpeg.comp_info[c];
        if(comp.h_samp_factor<1 || comp.h_samp_factor>4 || comp.v_samp_factor<1 || comp.v_samp_factor>4)
            throw FileFailure("open_failed","invalid JPEG sampling factors");
        max_h=std::max(max_h,unsigned(comp.h_samp_factor));
        max_v=std::max(max_v,unsigned(comp.v_samp_factor));
    }
    const auto columns=ceil_div(jpeg.image_width,8*max_h);
    const auto rows=ceil_div(jpeg.image_height,8*max_v);
    // Metadata copies plus a fixed allocator/table allowance and a
    // width-scaled allowance for multiple iMCU row buffers and alignment.
    auto bytes=checked_bytes_add(metadata_copies*header,4*bounded_mib+
        checked_bytes_mul(jpeg.image_width,8*max_v*jpeg.num_components*8));
    if(jpeg_has_multiple_scans(&jpeg)){
        for(int c=0;c<jpeg.num_components;++c) {
            const auto& comp=jpeg.comp_info[c];
            const auto block_columns=checked_bytes_mul(columns,comp.h_samp_factor);
            const auto block_rows=checked_bytes_mul(rows,comp.v_samp_factor);
            // libjpeg pads both dimensions to the component's MCU factors
            // and stores a pointer for each coefficient row.
            const auto row_bytes=checked_bytes_add(checked_bytes_mul(block_columns,sizeof(JBLOCK)),sizeof(void*));
            bytes=checked_bytes_add(bytes,checked_bytes_mul(row_bytes,block_rows));
        }
    }
    return bytes;
}
class JpegRows final : public NativeRowSource {
public:
    JpegRows(std::shared_ptr<Decoder> decoder,std::uint64_t budget):decoder_(std::move(decoder)) {
        auto& state=*decoder_;
        auto& jpeg=state.jpeg;
        if(state.started)throw FileFailure("decode_failed","JPEG decoder already started");
        state.started=true;
        const int width=jpeg.image_width,height=jpeg.image_height,channels=jpeg.num_components;
        if(setjmp(state.error.jump))throw FileFailure("decode_failed",state.error.message);
        jpeg.mem->max_memory_to_use=static_cast<long>(std::min<std::uint64_t>(budget,std::numeric_limits<long>::max()));
        if(!jpeg_start_decompress(&jpeg) || jpeg.output_width!=unsigned(width) ||
           jpeg.output_height!=unsigned(height) || jpeg.output_components!=channels)
            throw FileFailure("decode_failed","JPEG native layout changed during decode");
        stride_=std::size_t(width)*channels;
    }
    void read(int first,int last,void* destination) override {
        auto& state=*decoder_;
        auto& jpeg=state.jpeg;
        if(first<0 || first>=last || unsigned(first)!=jpeg.output_scanline || unsigned(last)>jpeg.output_height)
            throw FileFailure("decode_failed","JPEG rows must be read once in order");
        if(setjmp(state.error.jump))throw FileFailure("decode_failed",state.error.message);
        auto* pixels=static_cast<JSAMPLE*>(destination);
        while(jpeg.output_scanline<unsigned(last)) {
            JSAMPROW row=pixels;
            if(jpeg_read_scanlines(&jpeg,&row,1)!=1)throw FileFailure("decode_failed","incomplete JPEG scanline");
            pixels+=stride_;
        }
    }
    void finish() override {
        auto& state=*decoder_;
        if(setjmp(state.error.jump))throw FileFailure("decode_failed",state.error.message);
        if(!jpeg_finish_decompress(&state.jpeg))throw FileFailure("decode_failed","incomplete JPEG image");
    }
private:
    std::shared_ptr<Decoder> decoder_;
    std::size_t stride_=0;
};
}

std::optional<JpegProbe> probe_jpeg(const std::string& path,std::uint64_t budget,std::size_t icc_limit) {
    File file(OIIO::Filesystem::fopen(path,"rb"),&std::fclose);
    if(!file || std::getc(file.get())!=0xff || std::getc(file.get())!=0xd8)return std::nullopt;
    const auto header=header_bytes(file.get());
    if(header>budget/metadata_copies)throw FileFailure("decode_limit_exceeded","JPEG metadata exceeds decode budget");
    std::rewind(file.get());
    auto decoder=std::make_shared<Decoder>(std::move(file));
    decoder->header();
    auto& jpeg=decoder->jpeg;
    if(!jpeg.image_width || !jpeg.image_height || jpeg.image_width>JPEG_MAX_DIMENSION || jpeg.image_height>JPEG_MAX_DIMENSION ||
       jpeg.data_precision!=8 || (jpeg.num_components!=1 && jpeg.num_components!=3 && jpeg.num_components!=4))
        throw FileFailure("open_failed","unsupported JPEG dimensions or sample layout");
    JpegProbe result;
    result.multiscan=jpeg_has_multiple_scans(&jpeg);
    result.codec_bytes=decoder_bytes(jpeg,header);
    if(result.codec_bytes>budget)throw FileFailure("decode_limit_exceeded","JPEG coefficients and decoder scratch exceed decode budget");
    const bool cmyk=jpeg.jpeg_color_space==JCS_CMYK || jpeg.jpeg_color_space==JCS_YCCK;
    auto& spec=result.spec;
    spec=OIIO::ImageSpec(jpeg.image_width,jpeg.image_height,cmyk?3:jpeg.num_components,OIIO::TypeDesc::UINT8);
    spec.set_colorspace("srgb_rec709_scene");
    if(cmyk)spec.attribute("jpeg:ColorSpace",jpeg.jpeg_color_space==JCS_CMYK?"CMYK":"YCbCrK");
    if(jpeg.num_components>=3 && jpeg.comp_info[1].h_samp_factor==1 && jpeg.comp_info[1].v_samp_factor==1 &&
       jpeg.comp_info[2].h_samp_factor==1 && jpeg.comp_info[2].v_samp_factor==1) {
        const auto h=jpeg.comp_info[0].h_samp_factor,v=jpeg.comp_info[0].v_samp_factor;
        const char* sampling=h==1 && v==1?"4:4:4":h==2 && v==1?"4:2:2":h==2 && v==2?"4:2:0":h==4 && v==1?"4:1:1":nullptr;
        if(sampling)spec.attribute("jpeg:subsampling",sampling);
    }
    std::size_t icc_bytes=0;
    for(auto* marker=jpeg.marker_list;marker;marker=marker->next) {
        const auto* p=marker->data;
        const auto n=marker->data_length;
        if(marker->marker==JPEG_APP0+1){
            constexpr char xmp[]="http://ns.adobe.com/xap/1.0/";
            if(n>=6 && !std::memcmp(p,"Exif\0",6))OIIO::decode_exif(OIIO::cspan<std::uint8_t>(p+6,n-6),spec);
            else if(n>=sizeof(xmp) && !std::memcmp(p,xmp,sizeof(xmp)))
                OIIO::decode_xmp(OIIO::cspan<std::uint8_t>(p,n),spec);
        }
        if(marker->marker==JPEG_APP0+2 && n>=12 && !std::memcmp(p,"ICC_PROFILE",12)){
            result.has_icc=true;
            icc_bytes+=n>14?n-14:0;
        }
    }
    if(icc_bytes>icc_limit)throw CmsFailure("JPEG ICC segments exceed profile limit");
    if(result.has_icc){
        JOCTET* profile=nullptr;unsigned size=0;
        if(setjmp(decoder->error.jump))throw FileFailure("open_failed",decoder->error.message);
        if(jpeg_read_icc_profile(&jpeg,&profile,&size)){
            const std::unique_ptr<JOCTET,decltype(&std::free)> bytes(profile,&std::free);
            spec.attribute("ICCProfile",OIIO::TypeDesc(OIIO::TypeDesc::UINT8,int(size)),bytes.get());
        }
    }
    if(jpeg.jpeg_color_space==JCS_GRAYSCALE || jpeg.jpeg_color_space==JCS_RGB || jpeg.jpeg_color_space==JCS_YCbCr)
        result.read={unsigned(jpeg.num_components),0,result.codec_bytes,
            [decoder,budget=result.codec_bytes]{return std::make_unique<JpegRows>(decoder,budget);}};
    return result;
}
}
