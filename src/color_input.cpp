#include "color_input.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

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

bool container_has_icc(const std::string& filename) {
    std::ifstream file(std::filesystem::u8path(filename),std::ios::binary);
    std::array<unsigned char,8> header{};
    if(!file.read(reinterpret_cast<char*>(header.data()),header.size()))return false;
    auto byte=[&](){return file.get();};
    auto skip=[&](unsigned long long n){file.seekg(static_cast<std::streamoff>(n),std::ios::cur);};
    if(header==std::array<unsigned char,8>{137,80,78,71,13,10,26,10}) {
        while(file.read(reinterpret_cast<char*>(header.data()),header.size())) {
            // Only walk chunk headers, never allocate/decompress ICC payloads.
            if(std::memcmp(header.data()+4,"iCCP",4)==0)return true;
            if(std::memcmp(header.data()+4,"IEND",4)==0)return false;
            unsigned long long length=0;
            for(int i=0;i<4;++i)length=(length<<8)|header[i];
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
            file.seekg(static_cast<std::streamoff>(offset));
            const auto count=integer(big?8:2);
            const unsigned entry_size=big?20:12;
            if(count>(size-offset)/entry_size)return false;
            for(unsigned long long i=0;i<count && file;++i) {
                if(integer(2)==34675)return true; // Even an empty/invalid tag.
                skip(entry_size-2);
            }
            offset=integer(big?8:4);
        }
    } else if(std::memcmp(header.data(),"RIFF",4)==0) {
        char type[4];if(!file.read(type,4) || std::memcmp(type,"WEBP",4))return false;
        while(file.read(reinterpret_cast<char*>(header.data()),header.size())) {
            if(std::memcmp(header.data(),"ICCP",4)==0)return true;
            const unsigned long long length=header[4]|(static_cast<unsigned long long>(header[5])<<8)
                |(static_cast<unsigned long long>(header[6])<<16)|(static_cast<unsigned long long>(header[7])<<24);
            skip(length+(length&1));
        }
    } else if(header[0]==255 && header[1]==216) {
        file.seekg(2);
        while(file && byte()==255) {
            int marker=byte();while(marker==255)marker=byte();
            if(marker<0 || marker==0xda || marker==0xd9)return false;
            if(marker==0x01 || (marker>=0xd0 && marker<=0xd7))continue;
            const int high=byte(),low=byte();
            if(high<0 || low<0)return false;
            const int length=high*256+low-2;
            if(length<0)return false;
            if(marker==0xe2 && length>=12) {
                char prefix[12];if(!file.read(prefix,12))return false;
                if(std::memcmp(prefix,"ICC_PROFILE\0",12)==0)return true;
                skip(static_cast<unsigned>(length-12));
            } else skip(static_cast<unsigned>(length));
        }
    }
    return false;
}
}
