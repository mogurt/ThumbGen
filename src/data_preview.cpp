#include "data_preview.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace thumbgen::detail {
DataMapping data_mapping(const std::string& mode,const std::vector<double>& range,const std::string& invalid) {
    DataMapping result;
    if(mode=="colour")result.mode=DataMode::colour;
    else if(mode=="depth")result.mode=DataMode::depth;
    else if(mode=="normal")result.mode=DataMode::normal;
    else if(mode=="vector")result.mode=DataMode::vector;
    else if(mode=="id")result.mode=DataMode::id;
    else throw std::invalid_argument("unknown visualization mode");
    if(invalid=="black")result.invalid=InvalidData::black;
    else if(invalid=="magenta")result.invalid=InvalidData::magenta;
    else if(invalid=="error")result.invalid=InvalidData::error;
    else throw std::invalid_argument("invalid data value policy");
    if(result.mode==DataMode::colour || result.mode==DataMode::id) {
        if(!range.empty())throw std::invalid_argument("data range is only valid for depth, normal or vector");
        if(result.mode==DataMode::colour && invalid!="magenta")throw std::invalid_argument("invalid value policy requires a data visualization mode");
        return result;
    }
    if(range.empty()) {
        if(result.mode!=DataMode::normal)throw std::invalid_argument("depth and vector visualization require a fixed data range");
        result.minimum=-1;result.maximum=1;
    } else {
        if(range.size()!=2)throw std::invalid_argument("data range must contain two numbers");
        result.minimum=range[0];result.maximum=range[1];
    }
    const double extent=result.maximum-result.minimum;
    if(!std::isfinite(result.minimum) || !std::isfinite(result.maximum) || !std::isfinite(extent) || extent<=0)
        throw std::invalid_argument("data range must be finite and strictly increasing with a finite span");
    return result;
}
namespace {
void layout(std::size_t pixels,std::size_t stride,std::size_t component) {
    constexpr auto elements=std::numeric_limits<std::size_t>::max()/sizeof(float);
    if(!stride || stride>elements || component>=stride || pixels>elements/stride || pixels>elements/4)
        throw std::invalid_argument("invalid data buffer layout");
}
void palette(std::uint32_t id,float* rgba) noexcept {
    // Fixed integer operations: no process seed, colour transform or frame statistics.
    // Zero is reserved for a black background. Collisions are possible in RGB8.
    std::uint32_t h=id*2654435761u;
    h^=(h<<13)|(h>>19);h^=h>>17;
    for(unsigned c=0;c<3;++c)rgba[c]=id?float(64+((h>>(c*8))%192u))/255.0f:0.0f;
    rgba[3]=1;
}
float normalize(double value,const DataMapping& mapping,DataStats& stats) noexcept {
    if(value<mapping.minimum){++stats.clipped_components;return 0;}
    if(value>mapping.maximum){++stats.clipped_components;return 1;}
    return float((value-mapping.minimum)/(mapping.maximum-mapping.minimum));
}
template<class T> void ids(const T* source,float* rgba,std::size_t pixels,std::size_t stride,std::size_t component) {
    layout(pixels,stride,component);
    if(pixels && (!source || !rgba))throw std::invalid_argument("null data buffer");
    for(std::size_t p=0;p<pixels;++p)palette(static_cast<std::uint32_t>(source[p*stride+component]),rgba+4*p);
}
}
DataStats map_data_float(const float* source,float* rgba,std::size_t pixels,std::size_t stride,
                        const std::vector<int>& components,const DataMapping& mapping) {
    const auto size=components.size();
    bool shape=false;
    switch(mapping.mode) {
        case DataMode::depth:case DataMode::id:shape=size==1;break;
        case DataMode::normal:shape=size==3;break;
        case DataMode::vector:shape=size==2 || size==3;break;
        default:break;
    }
    if(!shape)throw std::invalid_argument("channel count does not match data visualization mode");
    if(mapping.invalid!=InvalidData::black && mapping.invalid!=InvalidData::magenta && mapping.invalid!=InvalidData::error)
        throw std::invalid_argument("invalid data value policy");
    for(int c:components){if(c<0)throw std::invalid_argument("negative data component");layout(pixels,stride,std::size_t(c));}
    if(pixels && (!source || !rgba))throw std::invalid_argument("null data buffer");
    if(mapping.mode!=DataMode::id && (!std::isfinite(mapping.minimum) || !std::isfinite(mapping.maximum) ||
       !std::isfinite(mapping.maximum-mapping.minimum) || mapping.maximum<=mapping.minimum))
        throw std::invalid_argument("invalid mapping range");
    DataStats stats;
    for(std::size_t p=0;p<pixels;++p) {
        double values[3]={0,0,0};bool valid=true;
        for(std::size_t c=0;c<size;++c){values[c]=source[p*stride+std::size_t(components[c])];valid=valid && std::isfinite(values[c]);}
        if(mapping.mode==DataMode::id)valid=valid && values[0]>=-2147483648.0 && values[0]<=4294967295.0 && std::trunc(values[0])==values[0];
        auto* target=rgba+4*p;
        if(!valid) {
            ++stats.invalid_pixels;const bool magenta=mapping.invalid==InvalidData::magenta;
            target[0]=target[2]=magenta?1.0f:0.0f;target[1]=0;target[3]=1;
            continue;
        }
        if(mapping.mode==DataMode::id)palette(static_cast<std::uint32_t>(static_cast<std::int64_t>(values[0])),target);
        else if(mapping.mode==DataMode::depth)target[0]=target[1]=target[2]=normalize(values[0],mapping,stats);
        else {
            for(std::size_t c=0;c<size;++c)target[c]=normalize(values[c],mapping,stats);
            if(size==2)target[2]=0.5f; // Missing third vector component is neutral.
        }
        target[3]=1;
    }
    if(mapping.invalid==InvalidData::error && stats.invalid_pixels)throw InvalidDataValues(stats);
    return stats;
}
void map_ids_u32(const std::uint32_t* source,float* rgba,std::size_t pixels,std::size_t stride,std::size_t component) {ids(source,rgba,pixels,stride,component);}
void map_ids_i32(const std::int32_t* source,float* rgba,std::size_t pixels,std::size_t stride,std::size_t component) {ids(source,rgba,pixels,stride,component);}
}
