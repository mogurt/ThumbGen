#pragma once
#include "colour_plan.h"
#include "bounded_read.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>
namespace thumbgen::detail {
// Pull adapter retaining exactly the global ColourPlan 1M-pixel calls and tail.
// Reader owns OIIO and translates read errors; this adapter never retries.
class C2PngRows {
    int width_,height_,channels_,decoded_=0;
    unsigned pixel_bytes_;
    std::function<void(int,int,void*)> read_;
    std::function<void(const void*,float*,std::size_t)> transform_;
    std::size_t total_,base_=0,count_=0,next_=0,row_used_=0,row_pixels_=0;
    std::vector<unsigned char> native_,row_;
    std::vector<float> linear_;
    void block(){
        base_=next_;count_=std::min(total_-next_,native_colour_block_pixels);
        if(!count_)throw std::runtime_error("C2 source exhausted");
        const auto pixel_bytes=pixel_bytes_;
        std::size_t used=0;
        while(used<count_){
            if(row_used_<row_pixels_){
                auto n=std::min(count_-used,row_pixels_-row_used_);
                std::memcpy(native_.data()+used*pixel_bytes,row_.data()+row_used_*pixel_bytes,n*pixel_bytes);
                used+=n;row_used_+=n;continue;
            }
            auto rows=(count_-used)/width_;
            if(rows){read_(decoded_,decoded_+int(rows),native_.data()+used*pixel_bytes);decoded_+=int(rows);used+=rows*width_;}
            else{if(decoded_>=height_)throw std::runtime_error("C2 native source exhausted");read_(decoded_,decoded_+1,row_.data());++decoded_;row_used_=0;row_pixels_=width_;}
        }
        transform_(native_.data(),linear_.data(),count_);next_+=count_;
    }
public:
    C2PngRows(int w,int h,int c,const ColourPlan& plan,std::function<void(int,int,void*)> read,
        std::function<void(const void*,float*,std::size_t)> transform):
        C2PngRows(w,h,c,c*plan.sample_bytes,std::move(read),std::move(transform)){}
    C2PngRows(int w,int h,int c,unsigned pixel_bytes,std::function<void(int,int,void*)> read,
        std::function<void(const void*,float*,std::size_t)> transform):width_(w),height_(h),channels_(c),pixel_bytes_(pixel_bytes),
        read_(std::move(read)),transform_(std::move(transform)),total_(std::size_t(w)*h),
        native_(std::min(total_,native_colour_block_pixels)*pixel_bytes),row_(std::size_t(w)*pixel_bytes),linear_(std::min(total_,native_colour_block_pixels)*c){}
    void get(int y,float* output){
        std::size_t pixel=std::size_t(y)*width_,remaining=width_;
        while(remaining){
            while(pixel>=base_+count_)block();
            if(pixel<base_ || pixel>=base_+count_)throw std::runtime_error("C2 source row rewind");
            auto n=std::min(remaining,base_+count_-pixel);
            std::memcpy(output,linear_.data()+(pixel-base_)*channels_,n*channels_*sizeof(float));
            pixel+=n;remaining-=n;output+=n*channels_;
        }
    }
    void finish(){
        // Consume any unused trailing pixels, retaining the original reader's
        // complete forward decode/error behavior rather than silently skipping.
        while(next_<total_)block();
        if(decoded_!=height_)throw std::runtime_error("C2 incomplete source decode");
    }
};
}
