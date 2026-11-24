#include "../src/other_rows.h"
#include "../src/files.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

namespace fs=std::filesystem;
namespace {
using namespace thumbgen::detail;
void require(bool ok,const std::string& reason) {
    if(!ok)throw std::runtime_error(reason);
}
struct Temp {
    fs::path path=fs::temp_directory_path()/("thumbgen-native-rows-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){fs::create_directory(path);}
    ~Temp(){std::error_code error;fs::remove_all(path,error);}
};

void compare_rows(const fs::path& path,OIIO::ImageSpec spec,int orientation,
    const char* compression,bool frame_buffered) {
    spec.attribute("Orientation",orientation);
    spec.attribute("compression",compression);
    spec.attribute("oiio:UnassociatedAlpha",1);
    auto out=OIIO::ImageOutput::create(path.u8string());
    require(out && out->open(path.u8string(),spec),"fixture open failed");
    std::vector<float> values(std::size_t(spec.width)*spec.height*spec.nchannels);
    for(int y=0;y<spec.height;++y)
        for(int x=0;x<spec.width;++x)
            for(int c=0;c<spec.nchannels;++c)
                values[(std::size_t(y)*spec.width+x)*spec.nchannels+c]=((x*11+y*7+c*45)%251)/250.0f;
    require(out->write_image(OIIO::TypeDesc::FLOAT,values.data()),out->geterror());
    require(out->close(),out->geterror());

    OIIO::ImageSpec config;
    config.attribute("oiio:UnassociatedAlpha",1);
    auto input=OIIO::ImageInput::open(path.u8string(),&config);
    require(bool(input),OIIO::geterror());
    const auto expected=input->spec();
    const auto probe=probe_other_rows(path.u8string(),fs::file_size(path),512*bounded_mib,
        config,std::numeric_limits<std::size_t>::max());
    require(probe && probe->spec && probe->read.open,"no bounded reader for "+path.filename().u8string());
    const auto& actual=*probe->spec;
    require(actual.width==expected.width && actual.height==expected.height &&
        actual.format==expected.format && actual.channelnames==expected.channelnames &&
        actual.get_string_attribute("oiio:ColorSpace")==expected.get_string_attribute("oiio:ColorSpace") &&
        actual.get_int_attribute("Orientation",1)==expected.get_int_attribute("Orientation",1) &&
        actual.get_int_attribute("oiio:UnassociatedAlpha",0)==expected.get_int_attribute("oiio:UnassociatedAlpha",0),
        "metadata differs from plugin");
    require(probe->read.pixel_bytes==actual.pixel_bytes() &&
        probe->read.retained_bytes==(frame_buffered?actual.image_bytes():0) &&
        probe->fallback_codec_bytes==probe->read.retained_bytes+probe->read.codec_bytes,
        "incorrect codec admission");
    std::vector<unsigned char> reference(actual.image_bytes()),pixels(actual.image_bytes());
    require(input->read_image(0,0,0,actual.nchannels,actual.format,reference.data()),input->geterror());
    input->close();
    auto reader=probe->read.open();
    bool refused=false;
    try {reader->read(1,2,pixels.data());}catch(const FileFailure& error){refused=error.code=="decode_failed";}
    require(refused,"accepted noncontiguous read");
    reader->read(0,2,pixels.data());
    reader->read(2,actual.height,pixels.data()+2*actual.scanline_bytes());
    reader->finish();
    require(pixels==reference,"bounded row samples differ from full native decode");
}
}

int main() try {
    Temp temp;
    using OIIO::ImageSpec;
    using OIIO::TypeDesc;
    compare_rows(temp.path/"source.hdr",ImageSpec(27,13,3,TypeDesc::FLOAT),2,"none",false);
    for(const int bits:{8,10,12,16}) {
        ImageSpec spec(27,13,3,bits==8?TypeDesc::UINT8:TypeDesc::UINT16);
        spec.attribute("oiio:BitsPerSample",bits);
        compare_rows(temp.path/("source-"+std::to_string(bits)+".dpx"),spec,2,"none",false);
    }
    compare_rows(temp.path/"source.bmp",ImageSpec(27,13,4,TypeDesc::UINT8),1,"none",false);
    compare_rows(temp.path/"source.tga",ImageSpec(27,13,4,TypeDesc::UINT8),1,"none",true);
    compare_rows(temp.path/"source.webp",ImageSpec(27,13,4,TypeDesc::UINT8),1,"webp:95",true);
    return 0;
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
