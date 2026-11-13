// Diagnostic only: lock the current resizer's operations before any streaming adaptation.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../third_party/stb/stb_image_resize2.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using Json=nlohmann::json;
static void require(bool b){if(!b)throw std::runtime_error("stb operation failed");}
static unsigned bits(float f){unsigned u;std::memcpy(&u,&f,4);return u;}
static int quant(float f){if(!(f>0))return 0;if(f>=1)return 255;float s=f*255;int b=int(s);return b+(s-b>=.5f);}
static void init(STBIR_RESIZE& r,const float* in,int w,int h,float* out,int ow,int oh) {
    stbir_resize_init(&r,in,w,h,0,out,ow,oh,0,STBIR_RGBA_PM,STBIR_TYPE_FLOAT);
    stbir_set_filters(&r,STBIR_FILTER_TRIANGLE,STBIR_FILTER_TRIANGLE);
    stbir_set_edgemodes(&r,STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP);
}
static std::vector<float> full(const std::vector<float>& in,int w,int h,int ow,int oh,int force=0) {
    stbir__force_vertical_first=force;
    std::vector<float> out(std::size_t(ow)*oh*4);STBIR_RESIZE r{};
    init(r,in.data(),w,h,out.data(),ow,oh);require(stbir_resize_extended(&r)!=0);stbir_free_samplers(&r);
    stbir__force_vertical_first=0;return out;
}
// The prior benchmark's exact double triangle + per-source scatter order.
static std::vector<float> old_stream(const std::vector<float>& in,int w,int h,int ow,int oh,Json* weight_log=nullptr) {
    std::vector<std::vector<std::pair<int,float>>> vertical(h);
    double scale=double(h)/oh,support=std::max(1.0,scale);
    for(int y=0;y<oh;++y) {
        double center=(y+.5)*scale-.5,sum=0;std::vector<std::pair<int,double>> weights;
        for(int i=int(std::ceil(center-support));i<=int(std::floor(center+support));++i) {
            double weight=std::max(0.0,1.0-std::abs(i-center)/support);
            if(weight>0){weights.emplace_back(std::clamp(i,0,h-1),weight);sum+=weight;}
        }
        for(auto [i,weight]:weights){float f=float(weight/sum);vertical[i].emplace_back(y,f);
            if(weight_log)weight_log->push_back({{"input",i},{"output",y},{"weight",f},{"bits",bits(f)}});}
    }
    std::vector<float> out(std::size_t(ow)*oh*4,0),horizontal(std::size_t(ow)*4);STBIR_RESIZE row{};
    init(row,in.data(),w,1,horizontal.data(),ow,1);require(stbir_build_samplers(&row)!=0);
    for(int y=0;y<h;++y) {
        stbir_set_buffer_ptrs(&row,in.data()+std::size_t(y)*w*4,0,horizontal.data(),0);
        require(stbir_resize_extended(&row)!=0);
        for(auto [target,weight]:vertical[y])for(int i=0;i<ow*4;++i)out[std::size_t(target)*ow*4+i]+=horizontal[i]*weight;
    }
    stbir_free_samplers(&row);return out;
}
static Json sampler(const stbir__sampler& s,bool all) {
    Json result={{"gather",s.is_gather},{"coefficient_width",s.coefficient_width},{"contributors",s.num_contributors},
        {"coefficients_bytes",s.coefficients_size},{"period",s.shared_period}};
    Json raw=Json::array();std::uint64_t hash=14695981039346656037ull;
    auto* data=reinterpret_cast<const unsigned char*>(s.coefficients);
    for(std::size_t i=0;i<s.coefficients_size;++i){hash^=data[i];hash*=1099511628211ull;}
    for(std::size_t i=0;i<(all?s.coefficients_size/4:std::min<std::size_t>(s.coefficients_size/4,64));++i){unsigned u;std::memcpy(&u,data+i*4,4);raw.push_back(u);}
    result["raw_coefficient_words"]=raw;result["coefficient_hash_fnv1a64"]=hash;
    Json ranges=Json::array();for(int i=0;i<(all?s.num_contributors:std::min(s.num_contributors,16));++i)
        ranges.push_back({s.contributors[i].n0,s.contributors[i].n1});
    result["ranges"]=ranges;return result;
}
static Json arithmetic(const std::vector<float>& in,int h,int oh,int output_y,const Json& old_weights) {
    std::vector<float> dummy(oh*4);STBIR_RESIZE r{};init(r,in.data(),1,h,dummy.data(),1,oh);
    require(stbir_build_samplers(&r)!=0);const auto& vertical=r.samplers->vertical;
    require(vertical.is_gather && !vertical.shared_period);
    auto range=vertical.contributors[output_y];
    const auto* weights=vertical.coefficients+std::size_t(output_y)*vertical.coefficient_width;
    float locked_sum=0,old_sum=0,merged_sum=0;std::vector<float> merged(h,0);
    Json locked_steps=Json::array(),old_steps=Json::array(),merged_steps=Json::array();
    for(const auto& item:old_weights)if(item["output"]==output_y) {
        const int y=item["input"];const float weight=item["weight"].get<float>();
        const float product=in[y*4+3]*weight;old_sum+=product;merged[y]+=weight;
        old_steps.push_back({{"input",y},{"weight_bits",bits(weight)},{"product_bits",bits(product)},{"sum_bits",bits(old_sum)}});
    }
    for(int y=range.n0;y<=range.n1;++y) {
        const auto weight=weights[y-range.n0];const float product=in[y*4+3]*weight;locked_sum+=product;
        locked_steps.push_back({{"input",y},{"weight_bits",bits(weight)},{"product_bits",bits(product)},{"sum_bits",bits(locked_sum)}});
    }
    for(int y=0;y<h;++y)if(merged[y]!=0) {
        const float product=in[y*4+3]*merged[y];merged_sum+=product;
        merged_steps.push_back({{"input",y},{"weight_bits",bits(merged[y])},{"product_bits",bits(product)},{"sum_bits",bits(merged_sum)}});
    }
    stbir_free_samplers(&r);
    return {{"old_duplicate_scatter",old_steps},{"locked_coefficients_serial",locked_steps},{"old_merged_clamp_weights",merged_steps},
        {"old_duplicate_sum_bits",bits(old_sum)},{"locked_serial_sum_bits",bits(locked_sum)},{"old_merged_sum_bits",bits(merged_sum)}};
}
struct Trace {int channels=4,last=-1,rewinds=0;Json calls=Json::array();};
static const void* input_cb(void*,const void* input,int n,int x,int y,void* ctx) {
    auto& t=*static_cast<Trace*>(ctx);if(y<t.last)++t.rewinds;t.last=y;t.calls.push_back({x,y,n});
    return static_cast<const float*>(input)+x*t.channels;
}
static Json trace(const std::vector<float>& in,int w,int h,int ow,int oh,bool all) {
    std::vector<float> out(std::size_t(ow)*oh*4);STBIR_RESIZE r{};Trace t;
    init(r,in.data(),w,h,out.data(),ow,oh);stbir_set_pixel_callbacks(&r,input_cb,nullptr);stbir_set_user_data(&r,&t);
    require(stbir_build_samplers(&r)!=0);auto* s=r.samplers;
    Json result={{"input",{w,h}},{"output",{ow,oh}},{"vertical_first",s->vertical_first},
        {"column_slices",s->num_column_slices},{"ring_entries",s->ring_buffer_num_entries},
        {"ring_entry_bytes",s->ring_buffer_length_bytes},{"sampler_alloced_total",s->alloced_total},
        {"horizontal",sampler(s->horizontal,all)},{"vertical",sampler(s->vertical,all)}};
    require(stbir_resize_extended(&r)!=0);result["rewinds"]=t.rewinds;result["calls_x_y_n"]=t.calls;
    stbir_free_samplers(&r);
    auto expected=full(in,w,h,ow,oh);result["callback_float_bits_equal"]=!std::memcmp(out.data(),expected.data(),out.size()*4);
    require(result["callback_float_bits_equal"].get<bool>());
    return result;
}
int main(){try {
    Json result;bool found=false;
    // First differing result, ascending input height, then output height,
    // then eight fixed patterns and exhaustive two-value binary patterns.
    // This minimizes within the stated family, not over every float array.
    // One-column alpha-only arrays eliminate horizontal and RGB operations.
    for(int h=2;h<=64 && !found;++h)for(int oh=1;oh<h && !found;++oh)for(int mode=0;mode<8+(h<=12?2*(1<<h):0) && !found;++mode) {
        std::vector<float> in(h*4,0);unsigned rng=1;
        for(int y=0;y<h;++y){rng=rng*1664525u+1013904223u;
            unsigned a=mode>=8?((mode-8)>=(1<<h)?(((mode-8)>>y)&1?65535:0):(((mode-8)>>y)&1?32767:32768)):mode==0?(y%2?65535:0):mode==1?(y==h/2?65535:0):mode==2?(y%2?32767:32768):
                mode==3?unsigned(y)*65535/(h-1):mode==4?(rng>>16):mode==5?32768:mode==6?21845:43690;
            in[y*4+3]=float(a)*(1.0f/65535.0f);}
        auto reference=full(in,1,h,1,oh),old=old_stream(in,1,h,1,oh);
        for(int y=0;y<oh;++y)if(quant(reference[y*4+3])!=quant(old[y*4+3])) {
            Json weights=Json::array();old_stream(in,1,h,1,oh,&weights);Json alpha=Json::array();
            for(int i=0;i<h;++i)alpha.push_back({{"value",in[i*4+3]},{"bits",bits(in[i*4+3])}});
            auto horizontal=full(in,1,h,1,oh,1);
            result["alpha_counterexample"]={{"input",{1,h}},{"output",{1,oh}},{"mode",mode},{"alpha",alpha},{"pixel_y",y},
                {"reference_bits",bits(reference[y*4+3])},{"old_bits",bits(old[y*4+3])},
                {"forced_horizontal_bits",bits(horizontal[y*4+3])},{"reference_alpha8",quant(reference[y*4+3])},
                {"old_alpha8",quant(old[y*4+3])},{"arithmetic",arithmetic(in,h,oh,y,weights)},{"old_vertical_weights",weights},{"locked_stb",trace(in,1,h,1,oh,true)}};
            found=true;break;
        }
    }
    require(found);
    // Pin the minimized example found by the documented small search. A future
    // resizer/FP change must be investigated instead of silently moving it.
    require(result["alpha_counterexample"]["input"]==Json::array({1,6}));
    require(result["alpha_counterexample"]["output"]==Json::array({1,1}));
    require(result["alpha_counterexample"]["reference_bits"]==1056964608u);
    require(result["alpha_counterexample"]["old_bits"]==1056964607u);
    result["access_traces"]=Json::array();
    const int sizes[][4]={{1,257,1,13},{257,1,13,1},{127,65,32,16},{4001,2000,425,212},{4001,2000,960,480},{8192,1024,960,120},{2048,4096,480,960}};
    for(auto& dims:sizes) {
        const int w=dims[0],h=dims[1],ow=dims[2],oh=dims[3];
        std::vector<float> in(std::size_t(w)*h*4);
        for(std::size_t i=0;i<in.size()/4;++i) {
            const float a=float((i*7919)%65536)*(1.0f/65535.0f);
            in[i*4]=a*.25f;in[i*4+1]=a*.5f;in[i*4+2]=a*.75f;in[i*4+3]=a;
        }
        result["access_traces"].push_back(trace(in,w,h,ow,oh,false));
    }
    std::cout<<result.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
