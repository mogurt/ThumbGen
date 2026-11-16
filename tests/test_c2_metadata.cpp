#include "c2_png_metadata.h"
#include <iostream>
#include <vector>
static void check(bool value){if(!value)throw std::runtime_error("C2 metadata contract failed");}
int main(){try{
    namespace fs=std::filesystem;
    const auto path=fs::temp_directory_path()/"thumbgen-c2-metadata-test.dat";
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ignored;fs::remove(path,ignored);}} cleanup{path};
    OIIO::ImageSpec spec(4001,2000,4,OIIO::TypeDesc::UINT16);
    auto write=[&](const char* type,unsigned length,unsigned repeats=1){
        std::ofstream file(path,std::ios::binary);std::vector<char> header(33);file.write(header.data(),header.size());
        std::vector<char> body(length+4);
        for(unsigned k=0;k<repeats;++k){unsigned char n[4]{static_cast<unsigned char>(length>>24),static_cast<unsigned char>(length>>16),static_cast<unsigned char>(length>>8),static_cast<unsigned char>(length)};
            file.write(reinterpret_cast<const char*>(n),4);file.write(type,4);file.write(body.data(),body.size());}
        const char idat[8]{0,0,0,0,'I','D','A','T'};file.write(idat,8);
    };
    auto eligible=[&](){return thumbgen::detail::c2_png_metadata_eligible(path,spec);};
    write("tEXt",12);check(eligible());
    spec.attribute("comment",std::string(2*1024*1024,'x'));check(!eligible());spec.erase_attribute("comment");
    // Even if spec has only one small overwritten key, libpng keeps both text
    // records. The raw total is checked independently from spec storage.
    write("tEXt",600000,2);check(!eligible());
    write("zTXt",12);check(!eligible());write("iTXt",12);check(!eligible());
    write("vpAg",12);check(!eligible());
    write("iCCP",16);check(eligible());write("iCCP",16,2);check(!eligible());
    write("iCCP",4*1024*1024+1);check(!eligible());
    write("tEXt",1,128);check(!eligible());write("tEXt",1,127);check(eligible());
    {std::ofstream file(path,std::ios::binary);std::vector<char> header(33);file.write(header.data(),header.size());
        const unsigned char length[4]{255,255,255,255};file.write(reinterpret_cast<const char*>(length),4);file.write("tEXt",4);}check(!eligible());
    {std::ofstream file(path,std::ios::binary);file<<"truncated";}check(!eligible());
    std::cout<<"C2 metadata: spec strings/capacity, retained duplicates, compressed text, ICC/chunk bounds and truncation passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
