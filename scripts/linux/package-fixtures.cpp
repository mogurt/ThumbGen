// Build-time validation utility; never included in a runtime candidate.
#include <OpenImageIO/imageio.h>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected a new fixture directory");
        const std::filesystem::path root(argv[1]);
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture directory already exists");
        for (const std::string extension : {"png", "exr"}) {
            const auto path = root / ("synthetic." + extension);
            auto writer = OIIO::ImageOutput::create(path.string());
            if (!writer) throw std::runtime_error(OIIO::geterror());
            OIIO::ImageSpec spec(12, 8, 3, extension == "exr" ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::UINT8);
            spec.attribute("oiio:ColorSpace", extension == "exr" ? "linear" : "sRGB");
            std::vector<float> pixels(12 * 8 * 3);
            for (size_t i = 0; i < pixels.size(); i += 3) {
                pixels[i] = 0.75f; pixels[i + 1] = 0.25f; pixels[i + 2] = 0.1f;
            }
            if (!writer->open(path.string(), spec) || !writer->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !writer->close())
                throw std::runtime_error(writer->geterror());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
