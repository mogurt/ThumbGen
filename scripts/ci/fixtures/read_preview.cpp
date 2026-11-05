#include <OpenImageIO/imageio.h>
#include <cmath>
#include <iostream>
#include <vector>

// Read every output pixel, rather than accepting only a decodable file header.
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    auto image = OIIO::ImageInput::open(argv[1]);
    if (!image) { std::cerr << OIIO::geterror(); return 1; }
    const auto& spec = image->spec();
    if (spec.width != std::stoi(argv[2]) || spec.height != std::stoi(argv[3]) ||
        spec.nchannels < 3 || spec.nchannels > 4) return 1;
    std::vector<float> pixels(spec.image_pixels() * spec.nchannels);
    if (!image->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data()) ||
        !image->close()) return 1;
    for (float value : pixels) if (!std::isfinite(value)) return 1;
    std::cout << spec.width << 'x' << spec.height << ": " << pixels.size() << " finite components\n";
    return 0;
}
