#include <OpenImageIO/imageio.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directory(root);
    std::vector<float> pixels(32 * 16 * 3);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) {
            const auto offset = static_cast<std::size_t>((y * 32 + x) * 3);
            pixels[offset] = float(x) / 31.0f;
            pixels[offset + 1] = float(y) / 15.0f;
            pixels[offset + 2] = 0.25f;
        }
    for (const std::string extension : std::array<std::string, 4>{"png", "exr", "jpg", "webp"}) {
        const auto path = (root / ("synthetic." + extension)).string();
        auto output = OIIO::ImageOutput::create(path);
        OIIO::ImageSpec spec(32, 16, 3, extension == "exr" ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::UINT8);
        if (!output || !output->open(path, spec) || !output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !output->close()) {
            std::cerr << "Fixture write failed: " << path << '\n';
            return 1;
        }
    }
    return 0;
}
