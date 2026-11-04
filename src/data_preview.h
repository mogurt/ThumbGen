#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <vector>

namespace thumbgen::detail {
enum class DataMode { colour,depth,normal,vector,id };
enum class InvalidData { black,magenta,error };
struct DataMapping {
    DataMode mode=DataMode::colour;
    InvalidData invalid=InvalidData::magenta;
    double minimum=0,maximum=1;
};
DataMapping data_mapping(const std::string& mode,const std::vector<double>& range,const std::string& invalid);
struct DataStats {
    std::uint64_t invalid_pixels=0,clipped_components=0;
};
class InvalidDataValues final : public std::runtime_error {
public:
    DataStats statistics;
    explicit InvalidDataValues(DataStats value):std::runtime_error("data visualization contains invalid samples"),statistics(value) {}
};
// Component offsets are relative to the decoded interleaved channel span.
// Source and destination buffers must not overlap. Error policy throws after
// mapping into the caller's temporary buffer; it must not be encoded on failure.
// FLOAT ID values must be integral and in [-2^31,2^32-1]. Native integer
// overloads preserve all 32 bits instead of passing IDs through FLOAT.
DataStats map_data_float(const float* source,float* rgba,std::size_t pixels,std::size_t stride,
    const std::vector<int>& components,const DataMapping& mapping);
void map_ids_u32(const std::uint32_t* source,float* rgba,std::size_t pixels,std::size_t stride,std::size_t component);
void map_ids_i32(const std::int32_t* source,float* rgba,std::size_t pixels,std::size_t stride,std::size_t component);
}
