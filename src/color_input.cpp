#include "color_input.h"
#include "cms_input.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <zlib.h>

namespace thumbgen::detail {
bool container_separated_colour(const std::string& filename) {
    // PSD/PSB's decoder does not preserve its original colour mode in ImageSpec.
    std::ifstream file(std::filesystem::u8path(filename),std::ios::binary);
    std::array<unsigned char,26> header{};
    if(!file.read(reinterpret_cast<char*>(header.data()),header.size()))return false;
    if(std::memcmp(header.data(),"8BPS",4)!=0)return false;
    const unsigned mode=unsigned(header[24])*256+header[25];
    return mode==4 || mode==7; // CMYK or Multichannel/separated inks.
}

bool separated_colour(const OIIO::ImageSpec& spec) {
    const auto jpeg=spec.get_string_attribute("jpeg:ColorSpace");
    const auto colour=spec.get_string_attribute("oiio:ColorSpace");
    return jpeg=="CMYK" || jpeg=="YCbCrK"
        || spec.get_int_attribute("tiff:PhotometricInterpretation",-1)==5
        || spec.get_string_attribute("tiff:ColorSpace")=="CMYK"
        || colour=="CMYK" || colour=="color separated";
}

bool container_has_icc(const std::string& filename,std::size_t limit) {
    std::ifstream file(std::filesystem::u8path(filename),std::ios::binary);
    std::array<unsigned char,8> header{};
    if(!file.read(reinterpret_cast<char*>(header.data()),header.size()))return false;
    auto byte=[&](){return file.get();};
    auto skip=[&](unsigned long long n){file.seekg(static_cast<std::streamoff>(n),std::ios::cur);};
    bool found=false;
    std::size_t jpeg_total=0;
    std::size_t tiff_total=0;
    if(header==std::array<unsigned char,8>{137,80,78,71,13,10,26,10}) {
        while(file.read(reinterpret_cast<char*>(header.data()),header.size())) {
            // Only walk chunk headers, never allocate/decompress ICC payloads.
            unsigned long long length=0;
            for(int i=0;i<4;++i)length=(length<<8)|header[i];
            if(std::memcmp(header.data()+4,"iCCP",4)==0) {
                found=true;
                if(length>limit)throw CmsFailure("compressed ICC chunk exceeds profile limit");
                if(limit!=std::numeric_limits<std::size_t>::max()) {
                    std::vector<unsigned char> compressed(static_cast<std::size_t>(length));
                    if(!file.read(reinterpret_cast<char*>(compressed.data()),static_cast<std::streamsize>(length)))throw CmsFailure("truncated iCCP");
                    const auto end=std::find(compressed.begin(),compressed.end(),0);
                    const auto start=std::size_t(end-compressed.begin())+2;
                    if(start>=compressed.size() || compressed[start-1]!=0)throw CmsFailure("invalid iCCP header");
                    std::vector<unsigned char> profile(limit);uLongf output=static_cast<uLongf>(limit);
                    if(uncompress(profile.data(),&output,compressed.data()+start,static_cast<uLong>(compressed.size()-start))!=Z_OK)
                        throw CmsFailure("invalid iCCP or expanded ICC exceeds profile limit");
                    skip(4);continue;
                }
            }
            if(std::memcmp(header.data()+4,"IEND",4)==0)return found;
            skip(length+4);
        }
    } else if((header[0]=='I' && header[1]=='I') || (header[0]=='M' && header[1]=='M')) {
        const bool little=header[0]=='I';
        auto integer=[&](int size) {
            unsigned long long value=0;
            for(int i=0;i<size;++i) {
                const int b=byte();if(b<0)return 0ULL;
                if(little)value|=static_cast<unsigned long long>(b)<<(8*i);
                else value=(value<<8)|static_cast<unsigned>(b);
            }
            return value;
        };
        file.seekg(2);const auto magic=integer(2);
        const bool big=magic==43;
        if(magic!=42 && !big)return false;
        if(big && (integer(2)!=8 || integer(2)!=0))return false;
        auto offset=integer(big?8:4);
        file.seekg(0,std::ios::end);const auto end=file.tellg();
        if(end<0)return false;
        const auto size=static_cast<unsigned long long>(end);
        std::set<unsigned long long> visited;
        while(file && offset && offset<size && visited.insert(offset).second) {
            if(limit!=std::numeric_limits<std::size_t>::max() && visited.size()>256)
                throw CmsFailure("ICC metadata scan exceeds 256 TIFF directories; explicit input_space required");
            file.seekg(static_cast<std::streamoff>(offset));
            const auto count=integer(big?8:2);
            const unsigned entry_size=big?20:12;
            if(count>(size-offset)/entry_size)return false;
            for(unsigned long long i=0;i<count && file;++i) {
                if(integer(2)==34675) {
                    found=true;integer(2);const auto bytes=integer(big?8:4);
                    if(bytes>limit-tiff_total)throw CmsFailure("TIFF ICC tags exceed aggregate profile limit");
                    tiff_total+=static_cast<std::size_t>(bytes);
                    skip(big?8:4);
                } else skip(entry_size-2);
            }
            offset=integer(big?8:4);
        }
    } else if(std::memcmp(header.data(),"RIFF",4)==0) {
        char type[4];if(!file.read(type,4) || std::memcmp(type,"WEBP",4))return false;
        while(file.read(reinterpret_cast<char*>(header.data()),header.size())) {
            const unsigned long long length=header[4]|(static_cast<unsigned long long>(header[5])<<8)
                |(static_cast<unsigned long long>(header[6])<<16)|(static_cast<unsigned long long>(header[7])<<24);
            if(std::memcmp(header.data(),"ICCP",4)==0) {
                found=true;if(length>limit)throw CmsFailure("WebP ICC chunk exceeds profile limit");
            }
            skip(length+(length&1));
        }
    } else if(header[0]==255 && header[1]==216) {
        file.seekg(2);
        while(file && byte()==255) {
            int marker=byte();while(marker==255)marker=byte();
            if(marker<0 || marker==0xda || marker==0xd9)return found;
            if(marker==0x01 || (marker>=0xd0 && marker<=0xd7))continue;
            const int high=byte(),low=byte();
            if(high<0 || low<0)return false;
            const int length=high*256+low-2;
            if(length<0)return false;
            if(marker==0xe2 && length>=12) {
                char prefix[12];if(!file.read(prefix,12))return false;
                if(std::memcmp(prefix,"ICC_PROFILE\0",12)==0) {
                    found=true;jpeg_total+=static_cast<unsigned>(std::max(0,length-14));
                    if(jpeg_total>limit)throw CmsFailure("JPEG ICC segments exceed profile limit");
                }
                skip(static_cast<unsigned>(length-12));
            } else skip(static_cast<unsigned>(length));
        }
    }
    return found;
}
}
