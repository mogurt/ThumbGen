#include "data_image.h"
#include "files.h"
#include <OpenImageIO/half.h>
#include <OpenImageIO/parallel.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <numeric>

namespace thumbgen::detail {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point tick){return std::chrono::duration<double,std::milli>(Clock::now()-tick).count();}
template<class T> double numeric(const unsigned char* data) {T value;std::memcpy(&value,data,sizeof(value));return static_cast<double>(value);}
double sample(const unsigned char* data,OIIO::TypeDesc type) {
    switch(type.basetype) {
        case OIIO::TypeDesc::UINT8:return numeric<std::uint8_t>(data);
        case OIIO::TypeDesc::INT8:return numeric<std::int8_t>(data);
        case OIIO::TypeDesc::UINT16:return numeric<std::uint16_t>(data);
        case OIIO::TypeDesc::INT16:return numeric<std::int16_t>(data);
        case OIIO::TypeDesc::UINT32:return numeric<std::uint32_t>(data);
        case OIIO::TypeDesc::INT32:return numeric<std::int32_t>(data);
        case OIIO::TypeDesc::UINT64:return numeric<std::uint64_t>(data);
        case OIIO::TypeDesc::INT64:return numeric<std::int64_t>(data);
        case OIIO::TypeDesc::HALF:return numeric<half>(data);
        case OIIO::TypeDesc::FLOAT:return numeric<float>(data);
        case OIIO::TypeDesc::DOUBLE:return numeric<double>(data);
        default:throw FileFailure("data_type_unsupported","unsupported native data channel type");
    }
}
}
std::size_t data_sample_bytes(const OIIO::ImageSpec& spec,const std::vector<int>& channels) {
    if(channels.empty() || std::any_of(channels.begin(),channels.end(),[&](int c){return c<0 || c>=spec.nchannels;}))throw FileFailure("visualization_layout_unsupported","invalid data channels");
    auto span=std::minmax_element(channels.begin(),channels.end());std::size_t maximum=1;
    for(int c=*span.first;c<=*span.second;++c) {
        auto type=spec.channelformat(c);unsigned char zero[8]={};
        if(type.size()>sizeof(zero) || type.aggregate!=OIIO::TypeDesc::SCALAR || type.arraylen)throw FileFailure("data_type_unsupported","unsupported native data channel type");
        sample(zero,type);maximum=std::max(maximum,type.size());
    }
    return maximum;
}
DataImage read_data_image(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const std::vector<int>& channels,const DataMapping& mapping,std::size_t limit,int threads) {
    data_sample_bytes(spec,channels);
    if(spec.width<1 || spec.height<1 || spec.depth!=1)throw FileFailure("invalid_dimensions","unsupported data image dimensions");
    auto span=std::minmax_element(channels.begin(),channels.end());const int first=*span.first,end=*span.second+1;
    std::vector<std::size_t> offsets;std::size_t stride=0;
    for(int c=first;c<end;++c){offsets.push_back(stride);stride+=spec.channelformat(c).size();}
    const auto pixels=std::size_t(spec.width)*std::size_t(spec.height);
    if(!stride || pixels>limit/stride)throw FileFailure("decode_limit_exceeded","native data source buffer exceeds limit");
    if(pixels>limit/(4*sizeof(float)))throw FileFailure("canvas_limit_exceeded","mapped RGBA data buffer exceeds limit");
    DataImage result;result.decoded_bytes=pixels*stride;
    auto tick=Clock::now();std::vector<unsigned char> native(result.decoded_bytes);
    if(!input.read_image(part,0,first,end,OIIO::TypeDesc::UNKNOWN,native.data()))throw FileFailure("decode_failed",input.geterror());
    result.decode_ms=elapsed(tick);tick=Clock::now();
    auto mapped=spec;mapped.nchannels=4;mapped.set_format(OIIO::TypeDesc::FLOAT);mapped.channelnames={"R","G","B","A"};mapped.alpha_channel=3;mapped.z_channel=-1;
    mapped.erase_attribute("ICCProfile");mapped.attribute("oiio:UnassociatedAlpha",0);
    result.image.reset(mapped);
    auto* target=static_cast<float*>(result.image.localpixels());
    if(!target || !result.image.contiguous())throw FileFailure("conversion_failed","cannot allocate mapped data image");
    std::vector<int> ordered(channels.size());std::iota(ordered.begin(),ordered.end(),0);
    map_data_double(nullptr,nullptr,0,ordered.size(),ordered,mapping); // Validate before parallel execution.
    std::atomic<std::uint64_t> invalid{0},clipped{0};std::exception_ptr failure;std::mutex mutex;
    auto nonthrowing=mapping;if(nonthrowing.invalid==InvalidData::error)nonthrowing.invalid=InvalidData::black;
    OIIO::parallel_for_chunked(0,spec.height,0,[&](int64_t begin,int64_t finish) {
        try {
            std::vector<double> row(std::size_t(spec.width)*channels.size());DataStats stats;
            for(auto y=begin;y<finish;++y) {
                for(int x=0;x<spec.width;++x)for(std::size_t c=0;c<channels.size();++c)
                    row[std::size_t(x)*channels.size()+c]=sample(native.data()+(std::size_t(y)*spec.width+x)*stride+offsets[channels[c]-first],spec.channelformat(channels[c]));
                auto one=map_data_double(row.data(),target+std::size_t(y)*spec.width*4,spec.width,channels.size(),ordered,nonthrowing);
                stats.invalid_pixels+=one.invalid_pixels;stats.clipped_components+=one.clipped_components;
            }
            invalid.fetch_add(stats.invalid_pixels);clipped.fetch_add(stats.clipped_components);
        }catch(...){std::lock_guard<std::mutex> guard(mutex);if(!failure)failure=std::current_exception();}
    },OIIO::paropt(threads,OIIO::paropt::SplitDir::Y,1));
    if(failure)std::rethrow_exception(failure);
    result.statistics={invalid.load(),clipped.load()};result.mapping_ms=elapsed(tick);
    if(mapping.invalid==InvalidData::error && result.statistics.invalid_pixels)throw InvalidDataValues(result.statistics);
    return result;
}
}
