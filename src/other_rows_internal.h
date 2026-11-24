#pragma once
#include "other_rows.h"
#include "files.h"
#include <array>
#include <cstring>
#include <fstream>

namespace thumbgen::detail::other_rows {
inline constexpr std::uint64_t metadata_limit=4*bounded_mib;
inline constexpr std::uint64_t metadata_copies=16;
inline constexpr std::uint64_t eager_limit=8*bounded_mib;
inline constexpr std::uint64_t plugin_bytes=bounded_mib;

using Byte=unsigned char;
inline bool is(const Byte* bytes,const char* literal,std::size_t size=4) {
    return std::memcmp(bytes,literal,size)==0;
}
inline unsigned le16(const Byte* b){return unsigned(b[0])|(unsigned(b[1])<<8);}
inline std::uint32_t le24(const Byte* b){return le16(b)|(std::uint32_t(b[2])<<16);}
inline std::uint32_t le32(const Byte* b){return le24(b)|(std::uint32_t(b[3])<<24);}
inline void require(bool ok,const char* message) {
    if(!ok)throw FileFailure("open_failed",message);
}
inline void reserve(std::uint64_t bytes,std::uint64_t budget,const char* message) {
    if(bytes>budget)throw FileFailure("decode_limit_exceeded",message);
}
inline std::uint64_t dimensions(std::uint64_t w,std::uint64_t h,unsigned limit,const char* message) {
    require(w && h,message);
    reserve(std::max(w,h),limit,message);
    return w*h;
}
struct Source {
    const std::string& path;
    std::uint64_t size,budget;
    const OIIO::ImageSpec& config;
    std::ifstream file;
    Source(const std::string& name,std::uint64_t bytes,std::uint64_t limit,const OIIO::ImageSpec& options)
        :path(name),size(bytes),budget(limit),config(options),file(std::filesystem::u8path(name),std::ios::binary){}
    void read(std::uint64_t offset,void* out,std::size_t bytes,const char* message) {
        require(offset<=size && bytes<=size-offset && offset<=std::uint64_t(std::numeric_limits<std::streamoff>::max()),message);
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        require(bool(file.read(static_cast<char*>(out),static_cast<std::streamsize>(bytes))),message);
    }
    void extent(std::uint64_t offset,std::uint64_t bytes,const char* message) const {
        require(offset<=size && bytes<=size-offset,message);
    }
};

OIIO::ImageSpec metadata(const Source&,const char* format);
OtherRowsProbe supported(const Source&,const char* format,OIIO::ImageSpec,
    std::uint64_t retained,std::uint64_t codec);
OtherRowsProbe fallback(const Source&,const char* format,std::uint64_t pixels,
    std::uint64_t codec,std::uint64_t eager=0);

OtherRowsProbe bmp(Source&);
OtherRowsProbe dpx(Source&);
OtherRowsProbe hdr(Source&);
OtherRowsProbe targa(Source&);
OtherRowsProbe webp(Source&,std::size_t icc_limit);
}
