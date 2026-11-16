#pragma once
#include "cms_input.h"
#include "c2_memory.h"
#include <OpenImageIO/imageio.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
namespace thumbgen::detail {
// Admission scope, not PNG validation. OIIO has already opened/identified the
// input. libpng retains text records even when ImageSpec overwrites duplicate
// names, so inspecting the final spec alone cannot bound retained metadata.
// Oversize/unknown compressed text keeps the existing complete route/errors.
inline bool c2_png_metadata_eligible(const std::filesystem::path& path,const OIIO::ImageSpec& spec){
    std::size_t bytes=0;
    auto add=[&](std::size_t n){if(n>c2_metadata_limit-bytes)return false;bytes+=n;return true;};
    if(spec.extra_attribs.capacity()>c2_metadata_limit/sizeof(OIIO::ParamValue) ||
       !add(spec.extra_attribs.capacity()*sizeof(OIIO::ParamValue)))return false;
    for(const auto& attribute:spec.extra_attribs){
        const int size=attribute.datasize();if(size<0 || !add(attribute.name().size()+1))return false;
        if(attribute.name()=="ICCProfile"){
            // The separate unchanged CMS reserve covers profile copies/parser.
            if(attribute.type().basetype!=OIIO::TypeDesc::UINT8 || std::size_t(size)>icc_profile_limit)return false;
            continue;
        }
        if(!add(std::size_t(size)))return false;
        if(attribute.type().basetype==OIIO::TypeDesc::STRING)
            for(auto value:attribute.as_cspan<OIIO::ustring>())if(!add(value.size()+1))return false;
    }
    std::ifstream file(path,std::ios::binary);file.seekg(33);
    std::uint64_t ancillary=0;bool profile=false;
    for(int chunks=0;chunks<128;++chunks){
        unsigned char header[8]{};if(!file.read(reinterpret_cast<char*>(header),8))return false;
        const std::uint64_t length=(std::uint32_t(header[0])<<24)|(std::uint32_t(header[1])<<16)|(std::uint32_t(header[2])<<8)|header[3];
        auto type=[&](const char* value){return std::memcmp(header+4,value,4)==0;};
        if(type("IDAT"))return true;
        if(type("IEND") || type("IHDR") || type("zTXt") || type("iTXt"))return false;
        if(type("iCCP")){
            if(profile || length>icc_profile_limit)return false;profile=true;
        }else{
            if(!(type("sRGB") || type("gAMA") || type("cHRM") || type("pHYs") || type("tIME") ||
                 type("oFFs") || type("sBIT") || type("bKGD") || type("tRNS") || type("tEXt")))return false;
            if(length+12>c2_metadata_limit-ancillary)return false;ancillary+=length+12;
        }
        // No CRC interpretation and no IDAT/tail scan: retain decoder behavior.
        file.seekg(std::streamoff(length+4),std::ios::cur);if(!file)return false;
    }
    return false;
}
}
