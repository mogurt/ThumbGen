#include "files.h"
#include "png_rows.h"
#include "tiff_rows.h"
#include <tiffio.h>
#include <chrono>
#include <iostream>

namespace fs=std::filesystem;
using namespace thumbgen::detail;
namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}

struct Temp {
    fs::path path=fs::temp_directory_path()/
        ("thumbgen-png-tiff-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() {fs::create_directory(path);}
    ~Temp() {std::error_code error;fs::remove_all(path,error);}
};

void write_png(const fs::path& path,int channels,OIIO::TypeDesc format,bool offset) {
    OIIO::ImageSpec spec(67,53,channels,format);
    if(offset){spec.x=5;spec.y=7;}
    spec.attribute("oiio:UnassociatedAlpha",1);
    auto file=OIIO::ImageOutput::create(path.u8string());
    require(file && file->open(path.u8string(),spec),"could not create PNG fixture");
    std::vector<unsigned char> row(std::size_t(spec.scanline_bytes()));
    for(int y=0;y<spec.height;++y) {
        for(std::size_t x=0;x<row.size();++x)row[x]=static_cast<unsigned char>(y*13+x*19);
        require(file->write_scanline(y+spec.y,0,format,row.data()),"could not write PNG fixture row");
    }
    require(file->close(),"could not finish PNG fixture");
}

void write_tiff(const fs::path& path,bool wide,bool tiled,int channels,int bits,int pages=1,
    const char* description=nullptr) {
#ifdef _WIN32
    auto* handle=TIFFOpenW(path.c_str(),wide?"w8b":"wl");
#else
    auto* handle=TIFFOpen(path.c_str(),wide?"w8b":"wl");
#endif
    std::unique_ptr<TIFF,decltype(&TIFFClose)> file(handle,TIFFClose);
    require(bool(file),"could not create TIFF fixture");
    constexpr int width=67,height=53,tile=16,strip=7;
    const auto pixel=std::size_t(channels)*(bits/8);
    for(int page=0;page<pages;++page) {
        TIFFSetField(handle,TIFFTAG_IMAGEWIDTH,width);
        TIFFSetField(handle,TIFFTAG_IMAGELENGTH,height);
        TIFFSetField(handle,TIFFTAG_SAMPLESPERPIXEL,channels);
        TIFFSetField(handle,TIFFTAG_BITSPERSAMPLE,bits);
        TIFFSetField(handle,TIFFTAG_PHOTOMETRIC,channels>=3?PHOTOMETRIC_RGB:PHOTOMETRIC_MINISBLACK);
        TIFFSetField(handle,TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
        TIFFSetField(handle,TIFFTAG_COMPRESSION,wide?COMPRESSION_ADOBE_DEFLATE:COMPRESSION_LZW);
        TIFFSetField(handle,TIFFTAG_PREDICTOR,PREDICTOR_HORIZONTAL);
        if(description)TIFFSetField(handle,TIFFTAG_IMAGEDESCRIPTION,description);
        if(channels==4 || channels==2) {
            std::uint16_t extra=wide?EXTRASAMPLE_UNASSALPHA:EXTRASAMPLE_ASSOCALPHA;
            TIFFSetField(handle,TIFFTAG_EXTRASAMPLES,1,&extra);
        }
        if(tiled) {
            TIFFSetField(handle,TIFFTAG_TILEWIDTH,tile);
            TIFFSetField(handle,TIFFTAG_TILELENGTH,tile);
            std::vector<unsigned char> block(tile*tile*pixel);
            for(int y=0;y<height;y+=tile)
                for(int x=0;x<width;x+=tile) {
                    for(std::size_t i=0;i<block.size();++i)
                        block[i]=static_cast<unsigned char>(x*5+y*7+i*17+page*31);
                    require(TIFFWriteTile(handle,block.data(),x,y,0,0)==tmsize_t(block.size()),
                        "could not write TIFF fixture tile");
                }
        } else {
            TIFFSetField(handle,TIFFTAG_ROWSPERSTRIP,strip);
            std::vector<unsigned char> row(width*pixel);
            for(int y=0;y<height;++y) {
                for(std::size_t i=0;i<row.size();++i)
                    row[i]=static_cast<unsigned char>(y*11+i*19+page*31);
                require(TIFFWriteScanline(handle,row.data(),y,0)==1,"could not write TIFF fixture row");
            }
        }
        require(TIFFWriteDirectory(handle)==1,"could not finish TIFF fixture directory");
    }
}

void compare_rows(const fs::path& path,bool png,int part) {
    OIIO::ImageSpec config;
    config.attribute("oiio:UnassociatedAlpha",1);
    auto input=OIIO::ImageInput::open(path.u8string(),&config);
    require(bool(input),"could not open fixture");
    OIIO::ImageSpec spec;
    require(input->seek_subimage(part,0,spec),"could not select fixture page");
    auto reference=OIIO::ImageInput::open(path.u8string(),&config);
    std::vector<unsigned char> expected(std::size_t(spec.image_bytes()));
    require(reference && reference->read_image(part,0,0,spec.nchannels,spec.format,expected.data()),
        "could not decode reference image");
    constexpr auto budget=64*bounded_mib;
    const auto plan=png?plan_png_rows(path,*input,spec,budget).read:plan_tiff_rows(path,spec,part,budget).read;
    require(bool(plan.open) && plan.pixel_bytes==spec.pixel_bytes(),"native fixture was not admitted");
    require(plan.retained_bytes<spec.image_bytes(),"native fixture retains a full frame");
    require(plan.codec_bytes>0,"native fixture omitted codec scratch");
    auto rows=plan.open();
    std::vector<unsigned char> actual(expected.size());
    for(int y=0;y<spec.height;y+=11)
        rows->read(y,std::min(y+11,spec.height),actual.data()+std::size_t(y)*spec.scanline_bytes());
    rows->finish();
    require(actual==expected,"native rows differ from complete decode");
    try {
        rows->read(0,1,actual.data());
        throw std::runtime_error("native reader allowed rewind");
    } catch(const FileFailure& error) {
        require(error.code=="decode_failed","native reader returned incorrect rewind error");
    }
}
}

int main() {try {
    Temp temp;
    const auto png=temp.path/"source.png",tiff=temp.path/"source.tif";
    for(int channels:{1,2,3,4}) {
        for(int bits:{8,16}) {
            write_png(png,channels,bits==16?OIIO::TypeDesc::UINT16:OIIO::TypeDesc::UINT8,channels==4);
            preflight_png_header(png,64*bounded_mib);
            compare_rows(png,true,0);
            for(bool big:{false,true})
                for(bool tiled:{false,true}) {
                    write_tiff(tiff,big,tiled,channels,bits,2);
                    preflight_tiff_header(tiff,64*bounded_mib);
                    compare_rows(tiff,false,0);
                    compare_rows(tiff,false,1);
                }
        }
    }
    const std::string description(2*bounded_mib,'m');
    write_tiff(tiff,true,false,3,8,1,description.c_str());
    try {
        preflight_tiff_header(tiff,bounded_mib);
        throw std::runtime_error("oversized TIFF directory was admitted");
    } catch(const FileFailure& error) {
        require(error.code=="decode_limit_exceeded","TIFF metadata limit returned the wrong error");
    }
    return 0;
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}}
