// Diagnostic OIIO/ColourPlan source, with original global 1M-pixel transforms.
#include "colour_plan.h"
#include <functional>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/color.h>
#include "quantize.h"
#include <filesystem>

static void finish_png(const float* pixels,const OIIO::ImageSpec& original,int w,int h,const std::string& path){
    auto spec=original;spec.x=spec.y=spec.z=spec.full_x=spec.full_y=spec.full_z=0;
    spec.width=spec.full_width=w;spec.height=spec.full_height=h;spec.depth=spec.full_depth=1;
    spec.set_format(OIIO::TypeDesc::FLOAT);spec.erase_attribute("ICCProfile");spec.attribute("oiio:UnassociatedAlpha",0);
    OIIO::ImageBuf image(spec);require(image.localpixels()!=nullptr);
    std::memcpy(image.localpixels(),pixels,size_t(w)*h*spec.nchannels*sizeof(float));
    OIIO::ColorConfig config("ocio://default");require(!config.has_error());
    require(OIIO::ImageBufAlgo::colorconvert(image,image,"Linear Rec.709 (sRGB)","sRGB Encoded Rec.709 (sRGB)",true,"","",&config));
    if(spec.nchannels==4){require(OIIO::ImageBufAlgo::unpremult(image,image));image.specmod().attribute("oiio:UnassociatedAlpha",1);}
    image.specmod().erase_attribute("compression");image.specmod().attribute("png:compressionLevel",3);
    image.specmod().attribute("png:filter",248);image.specmod().attribute("CompressionQuality",90);image.specmod().attribute("oiio:ColorSpace","sRGB");
    auto packed=image.spec();packed.set_format(OIIO::TypeDesc::UINT8);OIIO::ImageBuf output(packed);
    if(spec.nchannels==4)thumbgen::detail::quantize_rgba8(static_cast<const float*>(image.localpixels()),static_cast<unsigned char*>(output.localpixels()),size_t(w)*h);
    else thumbgen::detail::quantize_u8(static_cast<const float*>(image.localpixels()),static_cast<unsigned char*>(output.localpixels()),size_t(w)*h*spec.nchannels);
    require(output.write(path));
}
struct PngRows {
    OIIO::ImageInput::unique_ptr input;
    OIIO::ImageSpec spec;
    thumbgen::detail::ColourPlan plan;
    std::vector<unsigned char> native,row;
    std::vector<float> linear;
    size_t total=0,base=0,count=0,next=0,row_used=0,row_pixels=0;
    int decoded_y=0;
    explicit PngRows(const char* path){
        OIIO::ImageSpec config;config.attribute("oiio:UnassociatedAlpha",1);
        input=OIIO::ImageInput::open(path,&config);require(bool(input));spec=input->spec();
        require(std::string(input->format_name())=="png" && spec.depth==1 && (spec.nchannels==3 || spec.nchannels==4));
        bool embedded=spec.find_attribute("ICCProfile")!=nullptr;
        std::string space=spec.get_string_attribute("oiio:ColorSpace");if(space.empty())space="sRGB";
        plan=thumbgen::detail::colour_plan(spec,"png",true,true,embedded,embedded,space);
        require(plan.route==thumbgen::detail::ColourPlan::Route::integer_linear);
        total=size_t(spec.width)*spec.height;
        native.resize(std::min(total,size_t(1048576))*spec.nchannels*plan.sample_bytes);
        linear.resize(std::min(total,size_t(1048576))*spec.nchannels);
        row.resize(size_t(spec.width)*spec.nchannels*plan.sample_bytes);
    }
    void block(){
        base=next;count=std::min(total-next,size_t(1048576));require(count>0);
        const size_t pixel_bytes=spec.nchannels*plan.sample_bytes;
        size_t used=0;
        while(used<count){
            if(row_used<row_pixels){
                size_t n=std::min(count-used,row_pixels-row_used);
                std::memcpy(native.data()+used*pixel_bytes,row.data()+row_used*pixel_bytes,n*pixel_bytes);
                used+=n;row_used+=n;continue;
            }
            size_t rows=(count-used)/spec.width;
            if(rows){
                require(input->read_scanlines(0,0,decoded_y,decoded_y+int(rows),0,0,spec.nchannels,spec.format,native.data()+used*pixel_bytes));
                decoded_y+=int(rows);used+=rows*spec.width;
            }else{
                require(decoded_y<spec.height);
                require(input->read_scanlines(0,0,decoded_y,decoded_y+1,0,0,spec.nchannels,spec.format,row.data()));
                ++decoded_y;row_used=0;row_pixels=spec.width;
            }
        }
        plan.transform(native.data(),linear.data(),count,spec.nchannels);next+=count;
    }
    void get(int y,float* out){
        size_t pixel=size_t(y)*spec.width,remaining=spec.width;
        while(remaining){
            if(pixel>=base+count)block();
            require(pixel>=base && pixel<base+count);
            size_t n=std::min(remaining,base+count-pixel);
            std::memcpy(out,linear.data()+(pixel-base)*spec.nchannels,n*spec.nchannels*sizeof(float));
            pixel+=n;remaining-=n;out+=n*spec.nchannels;
        }
    }
    std::vector<float> full_reference(const char* path){
        // Independent reference decode, after the candidate has finished.
        // Read complete native source and use original transform(count) API.
        OIIO::ImageSpec config;config.attribute("oiio:UnassociatedAlpha",1);
        auto ref=OIIO::ImageInput::open(path,&config);require(bool(ref));
        std::vector<unsigned char> bytes(total*spec.nchannels*plan.sample_bytes);
        require(ref->read_image(0,0,0,spec.nchannels,spec.format,bytes.data()));ref->close();
        std::vector<float> result(total*spec.nchannels);
        plan.transform(bytes.data(),result.data(),total,spec.nchannels);return result;
    }
};
