// Benchmark read-back helper, not a product CLI command or a reference renderer.
#include <OpenImageIO/imageio.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    try {
        if(argc!=2)throw std::runtime_error("expected one image path");
        const auto path=std::filesystem::path(argv[1]);
        OIIO::attribute("threads",1);
        auto input=OIIO::ImageInput::open(path.u8string());
        if(!input)throw std::runtime_error("image cannot open: "+OIIO::geterror());
        const auto spec=input->spec();
        if(spec.width<1 || spec.height<1 || spec.depth!=1 || spec.nchannels<1 || spec.nchannels>4)
            throw std::runtime_error("unsupported preview shape");
        std::size_t count=std::size_t(spec.width);
        for(const auto factor:{std::size_t(spec.height),std::size_t(spec.nchannels)}) {
            if(count>16ULL*1024ULL*1024ULL/factor)throw std::runtime_error("preview validation exceeds 64 MiB");
            count*=factor;
        }
        std::vector<float> pixels(count);
        if(!input->read_image(0,0,0,spec.nchannels,OIIO::TypeDesc::FLOAT,pixels.data()))
            throw std::runtime_error("image cannot decode: "+input->geterror());
        if(!input->close())throw std::runtime_error("image close failed");
        const auto channels=std::min(3,spec.nchannels);
        std::vector<double> means(std::size_t(channels),0.0),maximum(std::size_t(channels),0.0);
        for(std::size_t pixel=0;pixel<count/std::size_t(spec.nchannels);++pixel)
            for(int channel=0;channel<channels;++channel) {
                const float value=pixels[pixel*std::size_t(spec.nchannels)+std::size_t(channel)];
                if(!std::isfinite(value))throw std::runtime_error("nonfinite decoded preview");
                means[std::size_t(channel)]+=value;maximum[std::size_t(channel)]=std::max(maximum[std::size_t(channel)],double(value));
            }
        for(auto& value:means)value/=double(count/std::size_t(spec.nchannels));
        nlohmann::json result{{"readable",true},{"width",spec.width},{"height",spec.height},{"channels",spec.nchannels},
            {"rgb_mean",means},{"rgb_max",maximum},{"all_black",*std::max_element(maximum.begin(),maximum.end())<=1.0/255.0}};
        std::cout<<result.dump()<<'\n'<<std::flush;return std::cout?0:1;
    } catch(const std::exception& error) {
        std::cout<<nlohmann::json({{"readable",false},{"error",error.what()}}).dump()<<'\n'<<std::flush;return 1;
    }
}
