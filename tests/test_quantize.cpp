#include "quantize.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static std::uint8_t reference(float value) {
    if(std::isnan(value) || value<=0)return 0;
    if(value>=1)return 255;
    // The product policy scales in binary32, then rounds half upward.
    // Double addition avoids changing a below-half float into an exact half.
    const float scaled=value*255.0f;
    return static_cast<std::uint8_t>(std::floor(static_cast<double>(scaled)+0.5));
}
int main() {
    try {
        std::vector<float> values={0.0f,-0.0f,1.0f,-1.0f,2.0f,
            std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::denorm_min(),
            0.28431373834609985f}; // Original production tie: scaled value 72.5 -> 73.
        for(int i=0;i<255;++i) {
            const float boundary=(float(i)+0.5f)/255.0f;
            values.push_back(std::nextafter(boundary,0.0f));
            values.push_back(boundary);
            values.push_back(std::nextafter(boundary,1.0f));
        }
        // Finite values, subnormals, signed infinities and NaN payloads.
        std::uint32_t state=0x1eaf1234;
        for(int i=0;i<200000;++i) {
            state=state*1664525u+1013904223u;
            float value;std::memcpy(&value,&state,sizeof(value));values.push_back(value);
        }
        const auto original=values;
        std::vector<std::uint8_t> expected(values.size());
        std::transform(values.begin(),values.end(),expected.begin(),reference);
        require(expected[9]==73,"production tie oracle");
        for(std::size_t chunk:{1,2,3,4,5,7,31,79,159,318,1275,65536}) {
            for(std::size_t offset=0;offset<8;++offset) {
                std::vector<float> input(offset+values.size());
                std::copy(values.begin(),values.end(),input.begin()+offset);
                std::vector<std::uint8_t> output(offset+values.size()+8,0xa5);
                for(std::size_t start=0;start<values.size();start+=chunk)
                    thumbgen::detail::quantize_u8(input.data()+offset+start,output.data()+offset+start,
                        std::min(chunk,values.size()-start));
                require(std::equal(expected.begin(),expected.end(),output.begin()+offset),"quantization differs by chunk or alignment");
                require(std::memcmp(input.data()+offset,original.data(),values.size()*sizeof(float))==0,"input buffer changed");
                require(std::all_of(output.begin(),output.begin()+offset,[](auto v){return v==0xa5;}) &&
                    std::all_of(output.end()-8,output.end(),[](auto v){return v==0xa5;}),"output canary overwritten");
            }
        }
        thumbgen::detail::quantize_u8(nullptr,nullptr,0);
        // Pixel chunks must retain every old byte at nonzero encoded Alpha,
        // including either side of the half-LSB transition and exceptional Alpha.
        std::vector<float> rgba;
        for(float alpha:values)rgba.insert(rgba.end(),{0.28431373834609985f,0.7f,1.0f,alpha});
        const auto rgba_original=rgba;
        std::vector<std::uint8_t> rgba_expected(rgba.size());
        std::transform(rgba.begin(),rgba.end(),rgba_expected.begin(),reference);
        for(std::size_t i=0;i<rgba_expected.size();i+=4)
            if(rgba_expected[i+3]==0)rgba_expected[i]=rgba_expected[i+1]=rgba_expected[i+2]=0;
        for(std::size_t pixels:{0,1,2,3,4,5,7,31,16385}) {
            if(!pixels) {thumbgen::detail::quantize_rgba8(nullptr,nullptr,0);continue;}
            for(std::size_t offset=0;offset<8;++offset) {
                std::vector<float> input(offset+rgba.size());
                std::copy(rgba.begin(),rgba.end(),input.begin()+offset);
                std::vector<std::uint8_t> output(offset+rgba.size()+8,0xa5);
                for(std::size_t start=0;start<values.size();start+=pixels)
                    thumbgen::detail::quantize_rgba8(input.data()+offset+start*4,output.data()+offset+start*4,
                        std::min(pixels,values.size()-start));
                require(std::equal(rgba_expected.begin(),rgba_expected.end(),output.begin()+offset),"RGBA storage policy or chunk mismatch");
                require(std::memcmp(input.data()+offset,rgba_original.data(),rgba.size()*sizeof(float))==0,"RGBA source changed");
                require(std::all_of(output.begin(),output.begin()+offset,[](auto v){return v==0xa5;}) &&
                    std::all_of(output.end()-8,output.end(),[](auto v){return v==0xa5;}),"RGBA canary overwritten");
            }
        }
        require(std::memcmp(values.data(),original.data(),values.size()*sizeof(float))==0,"source changed");
        std::cout<<values.size()<<" values, 12 chunk sizes, 8 alignments matched independent oracle\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
