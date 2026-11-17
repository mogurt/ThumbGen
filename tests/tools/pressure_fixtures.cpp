#include <OpenImageIO/imageio.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Case {
    std::string id, extension, compression;
    int width, height, channels;
    OIIO::TypeDesc type;
    bool sparse = false;
    int tile = 0, window = 0;
};

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if(argc != 2) throw std::runtime_error("expected a new output directory");
        const auto root = fs::absolute(fs::path(argv[1]));
        if(fs::exists(root)) throw std::runtime_error("output directory exists; left unchanged");
        fs::create_directories(root);
        OIIO::attribute("threads",1);
        using T = OIIO::TypeDesc;
        const std::vector<Case> cases{
            {"exr_rgb_half_zip","exr","zip",4096,2048,3,T::HALF},
            {"exr_rgb_float_piz","exr","piz",2048,1024,3,T::FLOAT},
            {"exr_many128_zip","exr","zip",1024,512,128,T::HALF},
            {"exr_many64_dwab","exr","dwab",1024,1024,64,T::HALF},
            {"exr_span32_zip","exr","zip",1024,1024,32,T::HALF,true},
            {"exr_span64_piz","exr","piz",1024,512,64,T::HALF,true},
            {"exr_span32_tiles","exr","zip",2048,512,32,T::FLOAT,true,64},
            {"exr_crop_large_display","exr","zip",256,256,4,T::HALF,false,0,1},
            {"exr_data_outside_display","exr","zip",2048,1024,4,T::HALF,false,0,2},
            {"tiff_rgba16_tiles","tif","zip",2048,2048,4,T::UINT16,false,256},
            {"tiff_rgba8_lzw","tif","lzw",3072,2048,4,T::UINT8},
            {"png_rgba16","png","zip",2048,2048,4,T::UINT16}
        };
        Json manifest=Json::array(), inventory=Json::array();
        for(const auto& c:cases) {
            OIIO::ImageSpec spec(c.width,c.height,c.channels,c.type);
            spec.attribute("compression",c.compression);
            spec.attribute("DateTime","2000:01:01 00:00:00");
            spec.attribute("oiio:ColorSpace",c.extension=="exr"?"linear":"sRGB");
            if(c.channels>4) {
                spec.channelnames.clear();
                for(int channel=0;channel<c.channels;++channel) {
                    if(c.sparse && channel==0) spec.channelnames.emplace_back("A");
                    else if(c.sparse && channel>=c.channels-3)
                        spec.channelnames.emplace_back(std::string("beauty.")+"RGB"[channel-(c.channels-3)]);
                    else if(!c.sparse && channel<4)spec.channelnames.emplace_back(std::string(1,"RGBA"[channel]));
                    else spec.channelnames.emplace_back("aaa"+std::to_string(1000+channel)+".X");
                }
                spec.alpha_channel=c.sparse?0:3;
            }
            if(c.tile) {spec.tile_width=c.tile;spec.tile_height=c.tile;spec.tile_depth=1;}
            if(c.window==1) {
                spec.full_x=-2048;spec.full_y=-1024;spec.full_width=4096;spec.full_height=2048;
                spec.x=-1536;spec.y=-256;
            } else if(c.window==2) {
                spec.x=-512;spec.y=-256;spec.full_x=0;spec.full_y=0;spec.full_width=1024;spec.full_height=512;
            }
            const auto count=std::size_t(c.width)*std::size_t(c.height)*std::size_t(c.channels);
            if(count>256ULL*1024ULL*1024ULL/sizeof(float)) throw std::runtime_error("fixture allocation exceeds 256 MiB");
            std::vector<float> pixels(count);
            std::uint32_t random=0x05a17e23;
            for(std::size_t index=0;index<count;++index) {
                random=random*1664525U+1013904223U;
                pixels[index]=float((random>>8)&0xffffU)/65535.0f;
            }
            if(spec.alpha_channel>=0)
                for(std::size_t pixel=0;pixel<std::size_t(c.width)*std::size_t(c.height);++pixel)
                    pixels[pixel*std::size_t(c.channels)+std::size_t(spec.alpha_channel)]=1.0f;
            const auto path=root/(c.id+"."+c.extension);
            auto writer=OIIO::ImageOutput::create(path.u8string());
            if(!writer || !writer->open(path.u8string(),spec) || !writer->write_image(T::FLOAT,pixels.data()) || !writer->close())
                throw std::runtime_error("fixture encode failed: "+path.u8string()+" "+OIIO::geterror());
            auto reader=OIIO::ImageInput::open(path.u8string());
            if(!reader) throw std::runtime_error("fixture cannot reopen");
            const auto actual=reader->spec();
            if(actual.width!=c.width || actual.height!=c.height || actual.nchannels!=c.channels)
                throw std::runtime_error("fixture dimensions/channel count changed");
            if(actual.format!=c.type || (c.extension!="png" && actual.get_string_attribute("compression")!=c.compression)
                || (c.tile && (actual.tile_width!=c.tile || actual.tile_height!=c.tile))
                || actual.x!=spec.x || actual.y!=spec.y || actual.full_x!=spec.full_x || actual.full_y!=spec.full_y
                || actual.full_width!=spec.full_width || actual.full_height!=spec.full_height)
                throw std::runtime_error("fixture storage/compression/window changed");
            int first=actual.nchannels,last=-1;
            for(int index=0;index<actual.nchannels;++index) {
                const auto& name=actual.channelnames[std::size_t(index)];
                if((c.sparse && (name=="A" || name.rfind("beauty.",0)==0)) ||
                   (!c.sparse && (name=="R" || name=="G" || name=="B" || name=="A"))) {
                    first=std::min(first,index);last=std::max(last,index);
                }
            }
            if(c.sparse && last-first+1<c.channels)throw std::runtime_error("requested sparse channel span collapsed");
            manifest.push_back({{"id",c.id},{"path",path.u8string()}});
            inventory.push_back({{"id",c.id},{"width",actual.width},{"height",actual.height},{"channels",actual.nchannels},
                {"selected_channel_span",last-first+1},{"compression",actual.get_string_attribute("compression")},
                {"sample_type",actual.format.c_str()},{"expect_nonblack",true},
                {"tile",{actual.tile_width,actual.tile_height}},{"data_window",{actual.x,actual.y,actual.width,actual.height}},
                {"display_window",{actual.full_x,actual.full_y,actual.full_width,actual.full_height}}});
            reader->close();
            std::cout<<c.id<<" channels="<<actual.nchannels<<" selected_span="<<last-first+1<<'\n';
        }
        for(const auto& item:std::vector<std::pair<std::string,Json>>{{"manifest.json",manifest},{"inventory.json",inventory}}) {
            std::ofstream stream(root/item.first);stream<<item.second.dump(2)<<'\n';stream.close();
            if(!stream)throw std::runtime_error("fixture manifest write failed");
        }
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
