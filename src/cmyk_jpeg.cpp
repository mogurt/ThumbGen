#include "cmyk_jpeg.h"
#include "cms_input.h"
#include <cstdio>
#include <csetjmp>
#include <filesystem>
#include <vector>
#include <memory>
#include <jpeglib.h>
#include <jerror.h>
namespace thumbgen::detail {
namespace {
struct Error {
    jpeg_error_mgr base;std::jmp_buf jump;char text[JMSG_LENGTH_MAX]{};
    std::size_t requested=0;
    void* (*small)(j_common_ptr,int,std::size_t)=nullptr;
    void* (*large)(j_common_ptr,int,std::size_t)=nullptr;
};
struct State {jpeg_decompress_struct decoder{};Error error{};};
void failed(j_common_ptr c){auto* error=reinterpret_cast<Error*>(c->err);c->err->format_message(c,error->text);std::longjmp(error->jump,1);}
void warning(j_common_ptr c,int level){if(level<0)failed(c);}
void reserve(j_common_ptr c,std::size_t size) {
    auto* error=static_cast<Error*>(c->client_data);
    if(size>cms_jpeg_limit-error->requested){c->err->msg_code=JERR_OUT_OF_MEMORY;c->err->msg_parm.i[0]=0;failed(c);}
    error->requested+=size;
}
void* small(j_common_ptr c,int pool,std::size_t size){reserve(c,size);return static_cast<Error*>(c->client_data)->small(c,pool,size);}
void* large(j_common_ptr c,int pool,std::size_t size){reserve(c,size);return static_cast<Error*>(c->client_data)->large(c,pool,size);}
}
void read_cmyk_jpeg(const std::string& path,int width,int height,float* pixels) {
    // All C++ owners are constructed before setjmp; no exceptions or C++
    // lifetimes cross libjpeg's C longjmp error boundary.
    std::vector<unsigned char> row(std::size_t(width)*4);
    // Mutable decoder/error state is on the heap, so longjmp does not leave
    // changed non-volatile automatic objects with indeterminate values.
    auto state=std::make_unique<State>();auto& decoder=state->decoder;auto& error=state->error;
#ifdef _WIN32
    FILE* file=nullptr;_wfopen_s(&file,std::filesystem::u8path(path).c_str(),L"rb");
#else
    FILE* file=std::fopen(path.c_str(),"rb");
#endif
    if(!file)throw CmsFailure("could not reopen raw CMYK JPEG");
    decoder.err=jpeg_std_error(&error.base);error.base.error_exit=failed;error.base.emit_message=warning;
    if(setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);std::fclose(file);
        throw CmsFailure(std::string("raw CMYK JPEG: ")+error.text);
    }
    jpeg_create_decompress(&decoder);decoder.mem->max_memory_to_use=static_cast<long>(cms_jpeg_limit);
    decoder.client_data=&error;error.small=decoder.mem->alloc_small;error.large=decoder.mem->alloc_large;
    decoder.mem->alloc_small=small;decoder.mem->alloc_large=large;
    jpeg_stdio_src(&decoder,file);jpeg_read_header(&decoder,TRUE);
    const bool supported=decoder.saw_Adobe_marker &&
        ((decoder.jpeg_color_space==JCS_CMYK && decoder.Adobe_transform==0) ||
         (decoder.jpeg_color_space==JCS_YCCK && decoder.Adobe_transform==2));
    if(!supported || decoder.progressive_mode || decoder.data_precision!=8 || decoder.image_width!=unsigned(width) || decoder.image_height!=unsigned(height)) {
        jpeg_destroy_decompress(&decoder);std::fclose(file);
        throw CmsFailure("raw CMYK JPEG requires baseline Adobe CMYK/YCCK with stable dimensions");
    }
    decoder.out_color_space=JCS_CMYK;jpeg_start_decompress(&decoder);
    while(decoder.output_scanline<decoder.output_height) {
        const auto y=decoder.output_scanline;JSAMPROW scan=row.data();
        if(jpeg_read_scanlines(&decoder,&scan,1)!=1)failed(reinterpret_cast<j_common_ptr>(&decoder));
        for(std::size_t x=0;x<row.size();++x)pixels[std::size_t(y)*row.size()+x]=1-float(row[x])/255;
    }
    jpeg_finish_decompress(&decoder);jpeg_destroy_decompress(&decoder);std::fclose(file);
}
}
