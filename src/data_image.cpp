#include "data_image.h"
#include "files.h"
#include "selected_input.h"
#include <OpenImageIO/half.h>
#include <OpenImageIO/parallel.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
bool valid_integer_ids(OIIO::TypeDesc type) noexcept {
    // Every value representable by these types is an integral supported ID.
    // UINT64/INT64 and floating storage still need the complete value scan.
    switch(type.basetype) {
        case OIIO::TypeDesc::UINT8:case OIIO::TypeDesc::INT8:
        case OIIO::TypeDesc::UINT16:case OIIO::TypeDesc::INT16:
        case OIIO::TypeDesc::UINT32:case OIIO::TypeDesc::INT32:return true;
        default:return false;
    }
}
}
DataImage read_data_image(OIIO::ImageInput& input,const OIIO::ImageSpec& spec,int part,
    const std::vector<int>& channels,const DataMapping& mapping,std::size_t limit,int threads) {
    if(spec.width<1 || spec.height<1 || spec.depth!=1)throw FileFailure("invalid_dimensions","unsupported data image dimensions");
    const auto plan=channel_plan(spec,channels,true);const auto stride=plan.pixel_bytes;
    const auto pixels=std::size_t(spec.width)*std::size_t(spec.height);
    if(!stride || pixels>limit/stride)throw FileFailure("decode_limit_exceeded","native data source buffer exceeds limit");
    const bool categorical=mapping.mode==DataMode::id || mapping.mode==DataMode::id_rgb;
    if(!categorical && pixels>limit/(4*sizeof(float)))throw FileFailure("canvas_limit_exceeded","mapped RGBA data buffer exceeds limit");
    DataImage result;result.decoded_bytes=pixels*stride;
    auto tick=Clock::now();std::vector<unsigned char> native(result.decoded_bytes);
    if(!read_selected(input,spec,part,plan,true,native.data()))throw FileFailure("decode_failed",input.geterror());
    result.decode_ms=elapsed(tick);tick=Clock::now();
    float* target=nullptr;
    if(!categorical) {
        auto mapped=spec;mapped.nchannels=4;mapped.set_format(OIIO::TypeDesc::FLOAT);mapped.channelnames={"R","G","B","A"};mapped.alpha_channel=3;mapped.z_channel=-1;
        mapped.erase_attribute("ICCProfile");mapped.attribute("oiio:UnassociatedAlpha",0);
        result.image.reset(mapped);target=static_cast<float*>(result.image.localpixels());
        if(!target || !result.image.contiguous())throw FileFailure("conversion_failed","cannot allocate mapped data image");
    }
    // Cache component metadata outside the per-pixel loop (mixed native types).
    for(int c:channels) {
        auto index=std::size_t(std::lower_bound(plan.channels.begin(),plan.channels.end(),c)-plan.channels.begin());
        result.offsets.push_back(plan.offsets[index]);result.strides.push_back(plan.strides[index]);result.types.push_back(spec.channelformat(c));
    }
    std::vector<int> ordered(channels.size());std::iota(ordered.begin(),ordered.end(),0);
    map_data_double(nullptr,nullptr,0,ordered.size(),ordered,mapping); // Validate before parallel execution.
    std::atomic<std::uint64_t> invalid{0},clipped{0};std::exception_ptr failure;std::mutex mutex;
    auto nonthrowing=mapping;if(nonthrowing.invalid==InvalidData::error)nonthrowing.invalid=InvalidData::black;
    const bool typed_ids=mapping.mode==DataMode::id && result.types.size()==1 && valid_integer_ids(result.types[0]);
    if(!typed_ids)OIIO::parallel_for_chunked(0,spec.height,0,[&](int64_t begin,int64_t finish) {
        try {
            std::vector<double> row(std::size_t(spec.width)*channels.size());DataStats stats;
            for(auto y=begin;y<finish;++y) {
                for(int x=0;x<spec.width;++x)for(std::size_t c=0;c<channels.size();++c)
                    row[std::size_t(x)*channels.size()+c]=sample(native.data()+(std::size_t(y)*spec.width+x)*result.strides[c]+result.offsets[c],result.types[c]);
                auto one=categorical?data_stats_double(row.data(),spec.width,channels.size(),ordered,nonthrowing):
                    map_data_double(row.data(),target+std::size_t(y)*spec.width*4,spec.width,channels.size(),ordered,nonthrowing);
                stats.invalid_pixels+=one.invalid_pixels;stats.clipped_components+=one.clipped_components;
            }
            invalid.fetch_add(stats.invalid_pixels);clipped.fetch_add(stats.clipped_components);
        }catch(...){std::lock_guard<std::mutex> guard(mutex);if(!failure)failure=std::current_exception();}
    },OIIO::paropt(threads,OIIO::paropt::SplitDir::Y,1));
    if(failure)std::rethrow_exception(failure);
    result.statistics={invalid.load(),clipped.load()};result.mapping_ms=elapsed(tick);
    if(mapping.invalid==InvalidData::error && result.statistics.invalid_pixels)throw InvalidDataValues(result.statistics);
    if(categorical){result.native=std::move(native);result.source_spec=spec;result.mapping=mapping;}
    return result;
}

CategoricalCoordinates categorical_coordinates(const OIIO::ImageSpec& spec,int width,int height) {
    if(width<1 || height<1 || spec.width<1 || spec.height<1 || spec.full_width<1 || spec.full_height<1)
        throw FileFailure("invalid_dimensions","invalid categorical preview dimensions");
    const int orientation=spec.get_int_attribute("Orientation",1);
    if(orientation<1 || orientation>8)throw FileFailure("orientation_failed","unsupported categorical orientation");
    int display_width=spec.full_width,display_height=spec.full_height;
    if(orientation>=5)std::swap(display_width,display_height);
    // Match OIIO nearest resample's binary32 pixel-center arithmetic. The old
    // path first pads to the display window and reorients at zero origin.
    auto axis=[](int output_size,int source_size) {
        std::vector<int> result(output_size);const float step=1.0f/float(output_size);
        for(int i=0;i<output_size;++i) {
            float t=(float(i)+0.5f)*step;float position=t*float(source_size);
            result[i]=double(position)>=double(source_size)?-1:int(std::floor(position));
        }
        return result;
    };
    auto xs=axis(width,display_width),ys=axis(height,display_height);
    CategoricalCoordinates result;result.transposed=orientation>=5;
    result.x=result.transposed?std::move(ys):std::move(xs);
    result.y=result.transposed?std::move(xs):std::move(ys);
    auto local=[](std::vector<int>& positions,int full,int origin,int data_origin,int data_size,bool flip) {
        for(auto& value:positions) {
            if(value<0 || value>=full){value=-1;continue;}
            auto position=std::int64_t(flip?full-1-value:value)+origin-data_origin;
            value=position<0 || position>=data_size?-1:int(position);
        }
    };
    local(result.x,spec.full_width,spec.full_x,spec.x,spec.width,orientation==2 || orientation==3 || orientation==7 || orientation==8);
    local(result.y,spec.full_height,spec.full_y,spec.y,spec.height,orientation==3 || orientation==4 || orientation==6 || orientation==7);
    return result;
}
OIIO::ImageBuf categorical_preview(const DataImage& image,int width,int height,std::size_t limit,int threads) {
    const auto& spec=image.source_spec;auto coordinates=categorical_coordinates(spec,width,height);
    if(image.native.empty() && !image.sampled)throw FileFailure("invalid_dimensions","missing categorical samples");
    if(std::size_t(width)>limit/(4*sizeof(float))/std::size_t(height))
        throw FileFailure("canvas_limit_exceeded","categorical preview exceeds pixel buffer limit");
    auto output_spec=spec;output_spec.x=output_spec.y=output_spec.z=0;
    output_spec.width=output_spec.full_width=width;output_spec.height=output_spec.full_height=height;
    output_spec.full_x=output_spec.full_y=output_spec.full_z=0;output_spec.depth=output_spec.full_depth=1;
    output_spec.nchannels=4;output_spec.set_format(OIIO::TypeDesc::FLOAT);output_spec.channelnames={"R","G","B","A"};
    output_spec.alpha_channel=3;output_spec.z_channel=-1;
    if(spec.get_int_attribute("Orientation",1)!=1)output_spec.attribute("Orientation",1);
    output_spec.erase_attribute("ICCProfile");output_spec.attribute("oiio:UnassociatedAlpha",0);
    OIIO::ImageBuf output(output_spec);auto* pixels=static_cast<float*>(output.localpixels());
    if(!pixels || !output.contiguous())throw FileFailure("conversion_failed","cannot allocate categorical preview");
    if(image.sampled) {
        auto compact=[](std::vector<int>& positions,const std::vector<int>& selected) {
            for(auto& value:positions)if(value>=0) {
                auto it=std::lower_bound(selected.begin(),selected.end(),value);
                if(it==selected.end() || *it!=value)throw FileFailure("conversion_failed","categorical position was not sampled");
                value=int(it-selected.begin());
            }
        };
        compact(coordinates.x,image.sampled_x);compact(coordinates.y,image.sampled_y);
    }
    const auto row_width=image.sampled?image.sampled_x.size():std::size_t(spec.width);
    std::vector<int> ordered(image.offsets.size());std::iota(ordered.begin(),ordered.end(),0);
    std::exception_ptr failure;std::mutex mutex;
    OIIO::parallel_for_chunked(0,height,0,[&](int64_t begin,int64_t end) {
        try {
            for(auto y=begin;y<end;++y)for(int x=0;x<width;++x) {
                auto* target=pixels+(std::size_t(y)*width+x)*4;
                int dx=coordinates.x[coordinates.transposed?std::size_t(y):std::size_t(x)];
                int dy=coordinates.y[coordinates.transposed?std::size_t(x):std::size_t(y)];
                if(dx<0 || dy<0){std::fill_n(target,4,0.0f);continue;}
                auto pixel=std::size_t(dy)*row_width+std::size_t(dx);
                double values[3]={};for(std::size_t c=0;c<image.offsets.size();++c)
                    values[c]=sample(image.native.data()+pixel*image.strides[c]+image.offsets[c],image.types[c]);
                map_data_double(values,target,1,image.offsets.size(),ordered,image.mapping);
            }
        }catch(...){std::lock_guard<std::mutex> guard(mutex);if(!failure)failure=std::current_exception();}
    },OIIO::paropt(threads,OIIO::paropt::SplitDir::Y,1));
    if(failure)std::rethrow_exception(failure);
    return output;
}
}
