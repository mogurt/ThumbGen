#include "c2_png_rows.h"
#include <iostream>
#include <stdexcept>
static void require(bool b){if(!b)throw std::runtime_error("C2 colour block contract failed");}
static void check(int w,int h,bool skip){
    thumbgen::detail::ColourPlan plan;plan.route=thumbgen::detail::ColourPlan::Route::integer_linear;plan.sample_bytes=2;
    int decoded=0;size_t transformed=0,calls=0;
    auto value=[](size_t index){return std::uint16_t(index*7919u);};
    thumbgen::detail::C2PngRows rows(w,h,3,plan,[&](int first,int last,void* output){
        require(first==decoded && first<last && last<=h);decoded=last;
        auto* p=static_cast<std::uint16_t*>(output);
        for(size_t i=0;i<size_t(last-first)*w*3;++i)p[i]=value(size_t(first)*w*3+i);
    },[&](const void* source,float* output,size_t pixels){
        require(pixels==std::min(size_t(1048576),size_t(w)*h-transformed));++calls;
        const auto* p=static_cast<const std::uint16_t*>(source);
        for(size_t i=0;i<pixels*3;++i){require(p[i]==value(transformed*3+i));output[i]=float(p[i])*(1.0f/65535);}
        transformed+=pixels;
    });
    std::vector<float> row(size_t(w)*3);
    for(int y=skip?h-1:0;y<h;++y){
        rows.get(y,row.data());
        for(size_t i=0;i<row.size();++i)require(row[i]==float(value(size_t(y)*w*3+i))*(1.0f/65535));
    }
    rows.finish();require(decoded==h && transformed==size_t(w)*h && calls==(size_t(w)*h+1048575)/1048576);
}
int main(){try{
    check(4001,2000,false);check(4001,2000,true);check(1048577,2,false);check(1,6,false);
    thumbgen::detail::ColourPlan plan;plan.sample_bytes=2;
    int reads=0;bool failed=false;
    try{thumbgen::detail::C2PngRows rows(4001,2000,3,plan,[&](int,int,void*){++reads;throw std::logic_error("read failure");},[](const void*,float*,size_t){});std::vector<float> row(4001*3);rows.get(0,row.data());}
    catch(const std::logic_error&){failed=true;}
    require(failed && reads==1);
    std::cout<<"C2 rows: global chunks/tail, odd/wide rows, forward skips and no retry passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
