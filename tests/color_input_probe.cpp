// Observe decoder output only; expected samples are authored independently.
#include <OpenImageIO/imageio.h>
#include "color_input.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
static int emit(const nlohmann::json& result) {
    // Complete the protocol write before DLL/CRT teardown. With the locked
    // Conda DLL closure, relying on exit-time flushing can silently lose it.
    std::cout<<result.dump()<<'\n'<<std::flush;
    if(!std::cout){std::cerr<<"could not flush probe JSON to stdout\n";return 1;}
    return 0;
}
int main(int argc,char** argv) {
    if(argc<2)return 2;
    if(argc==3 && std::string(argv[1])=="--presence") {
        return emit({{"icc",thumbgen::detail::container_has_icc(argv[2])}});
    }
    OIIO::ImageSpec config;
    config.attribute("oiio:RawColor",argc>2?1:0);
    auto in=OIIO::ImageInput::open(argv[1],&config);
    if(!in){std::cerr<<OIIO::geterror();return 1;}
    const auto spec=in->spec();
    std::vector<float> pixels(spec.image_pixels()*spec.nchannels);
    if(!in->read_image(0,0,0,spec.nchannels,OIIO::TypeDesc::FLOAT,pixels.data())){std::cerr<<in->geterror();return 1;}
    nlohmann::json result={{"channels",spec.channelnames},{"icc",spec.find_attribute("ICCProfile")!=nullptr},
        {"jpeg",std::string(spec.get_string_attribute("jpeg:ColorSpace"))},
        {"tiff",std::string(spec.get_string_attribute("tiff:ColorSpace"))},
        {"space",std::string(spec.get_string_attribute("oiio:ColorSpace"))},
        {"first",std::vector<float>(pixels.begin(),pixels.begin()+spec.nchannels)}};
    if(!in->close())return 1;
    return emit(result);
}
