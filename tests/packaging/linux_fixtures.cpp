// Authored package fixtures; build-time tool, never shipped.
#include <OpenImageIO/imageio.h>
#include <filesystem>
#include <stdexcept>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::create_directory(argv[1])) return 1;
    const std::filesystem::path root(argv[1]);
    for (const char* extension : {"png", "jpg", "webp", "exr"}) {
        OIIO::ImageSpec spec(12, 8, 3, OIIO::TypeDesc::UINT8);
        spec.attribute("oiio:ColorSpace", "sRGB");
        std::vector<float> pixels(12*8*3);
        for (int y=0; y<8; ++y) for (int x=0; x<12; ++x) {
            auto i=(y*12+x)*3;
            pixels[i]=.1f+.8f*x/11; pixels[i+1]=.1f+.8f*y/7; pixels[i+2]=.25f;
        }
        auto writer=OIIO::ImageOutput::create((root/(std::string("synthetic.")+extension)).string());
        if (!writer || !writer->open((root/(std::string("synthetic.")+extension)).string(),spec) ||
            !writer->write_image(OIIO::TypeDesc::FLOAT,pixels.data()) || !writer->close()) return 2;
    }
    OIIO::ImageSpec spec(8,2,4,OIIO::TypeDesc::FLOAT);
    spec.channelnames={"R","G","B","A"}; spec.alpha_channel=3;
    spec.attribute("oiio:ColorSpace","linear");
    std::vector<float> pixels(8*2*4,0);
    for(int y=0;y<2;++y) pixels[(y*8+3)*4+3]=1;
    auto path=(root/"lanczos-alpha.exr").string();
    auto writer=OIIO::ImageOutput::create(path);
    return !writer || !writer->open(path,spec) || !writer->write_image(OIIO::TypeDesc::FLOAT,pixels.data()) || !writer->close();
}
