#include "c2_resize.h"
#include "c2_memory.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../third_party/stb/stb_image_resize2.h"
static void require(bool value){if(!value)throw std::runtime_error("C2 adapter mismatch");}
static void pair(int w,int h,int ow,int oh,int channels,unsigned seed){
    std::vector<float> input(size_t(w)*h*channels);
    for(size_t i=0;i<size_t(w)*h;++i){
        unsigned v=unsigned(i)*1664525u+seed*1013904223u;
        float alpha=seed?float(v>>16)*(1.0f/65535):float(i<3?32767:32768)*(1.0f/65535);
        if(seed && seed%5==0 && i%3==0)alpha=0;
        for(int c=0;c<channels;++c)input[i*channels+c]=c==3?alpha:seed?alpha*float((v>>(c*5))&255)*(1.0f/255):0;
    }
    std::vector<std::pair<int,int>> dimensions{{ow,oh},{std::max(1,ow/2),std::max(1,oh/2)}};
    std::vector<std::vector<float>> output;std::vector<float*> pointers;
    for(auto [dw,dh]:dimensions)output.emplace_back(size_t(dw)*dh*channels);
    for(auto& data:output)pointers.push_back(data.data());
    thumbgen::detail::C2Resize adapter(w,h,channels,dimensions);
    require(adapter.workspace_bytes()<=thumbgen::detail::c2_workspace_limit);
    int previous=-1;
    adapter.execute([&](int y,float* row){require(y>previous);previous=y;std::memcpy(row,input.data()+size_t(y)*w*channels,size_t(w)*channels*sizeof(float));},pointers);
    for(size_t k=0;k<dimensions.size();++k){
        auto [dw,dh]=dimensions[k];std::vector<float> expected(output[k].size());
        require(stbir_resize(input.data(),w,h,0,expected.data(),dw,dh,0,channels==4?STBIR_RGBA_PM:STBIR_RGB,STBIR_TYPE_FLOAT,STBIR_EDGE_CLAMP,STBIR_FILTER_TRIANGLE));
        require(std::memcmp(expected.data(),output[k].data(),expected.size()*sizeof(float))==0);
    }
    bool threw=false;
    try{adapter.execute([](int,float*){},pointers);}catch(const std::runtime_error&){threw=true;}
    require(threw);
}
int main(){try{
    bool limited=false;
    try{thumbgen::detail::C2Resize impossible(2147483647,1,4,{{1,1}});}
    catch(const thumbgen::detail::C2PlanLimit&){limited=true;}
    require(limited);
    pair(1,6,1,1,4,0);
    const int shapes[][4]={{1,257,1,13},{257,1,13,1},{127,65,32,16},{4001,2000,425,212},{4001,2000,960,480},{8192,1024,960,120},{2048,4096,480,960},{7,9,7,9}};
    for(auto& shape:shapes)for(int c:{3,4})pair(shape[0],shape[1],shape[2],shape[3],c,1);
    for(unsigned i=1;i<=216;++i){int w=1+(i*7919)%513,h=1+(i*3571)%271;pair(w,h,1+(i*13)%w,1+(i*7)%h,i%2?3:4,i);}
    for(int at:{0,19,1999}){
        thumbgen::detail::C2Resize adapter(4001,2000,4,{{425,212},{960,480}});
        std::vector<float> a(425*212*4),b(960*480*4);bool caught=false;
        try{adapter.execute([&](int y,float* row){if(y>=at)throw std::logic_error("injected source failure");std::fill(row,row+4001*4,0.0f);},{a.data(),b.data()});}
        catch(const std::logic_error&){caught=true;}
        require(caught);
    }
    std::cout<<"C2 product adapter: 233 same-platform bit-exact dual pairs, one-shot and source failures passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
