#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using OIIO::ImageSpec;
using OIIO::TypeDesc;
using thumbgen::Json;
static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot snapshot source");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static void write(const fs::path& path, const ImageSpec& spec, const std::vector<float>& pixels) {
    auto output = OIIO::ImageOutput::create(path.u8string());
    require(bool(output), "fixture writer missing: " + OIIO::geterror());
    require(output->open(path.u8string(), spec), "fixture open: " + output->geterror());
    require(output->write_image(TypeDesc::FLOAT, pixels.data()), "fixture write: " + output->geterror());
    require(output->close(), "fixture close: " + output->geterror());
}
static float srgb(float linear) {
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

struct Fixture {
    std::string extension;
    TypeDesc type = TypeDesc::UINT8;
    int channels = 3;
    std::string compression;
    bool linear = false;
    int bits = 0;
    bool mixed = false;
};
static Fixture fixture(const std::string& name) {
    if (name == "jpeg_rgb8") return {"jpg"};
    if (name == "jpeg_gray8") return {"jpg", TypeDesc::UINT8, 1};
    if (name == "png_rgb8") return {"png"};
    if (name == "png_rgb16") return {"png", TypeDesc::UINT16};
    if (name == "png_rgba16") return {"png", TypeDesc::UINT16, 4};
    if (name == "png_gray8") return {"png", TypeDesc::UINT8, 1};
    if (name == "png_grayalpha16") return {"png", TypeDesc::UINT16, 2};
    if (name == "tiff_rgb8") return {"tif"};
    if (name == "tiff_rgb16") return {"tif", TypeDesc::UINT16};
    if (name == "tiff_rgbfloat") return {"tif", TypeDesc::FLOAT, 3, "zip", true};
    if (name == "tiff_rgba16") return {"tif", TypeDesc::UINT16, 4};
    if (name == "tga_rgb8") return {"tga"};
    if (name == "tga_rgba8") return {"tga", TypeDesc::UINT8, 4};
    if (name == "bmp_rgb8") return {"bmp"};
    if (name == "hdr_rgbfloat") return {"hdr", TypeDesc::FLOAT, 3, "", true};
    if (name == "dpx_rgb10") return {"dpx", TypeDesc::UINT16, 3, "", true, 10};
    if (name == "exr_float") return {"exr", TypeDesc::FLOAT, 3, "zip", true};
    if (name == "exr_mixed") return {"exr", TypeDesc::HALF, 4, "zip", true, 0, true};
    if (name.rfind("exr_half_", 0) == 0) return {"exr", TypeDesc::HALF, 3, name.substr(9), true};
    throw std::runtime_error("unknown matrix case: " + name);
}

static Json format_case(const fs::path& root, const std::string& name) {
    const auto f = fixture(name);
    auto input = root / ("source." + f.extension);
    ImageSpec spec(16, 8, f.channels, f.type);
    if (f.channels == 1) spec.channelnames = {"Y"};
    if (f.channels == 2) { spec.channelnames = {"Y", "A"}; spec.alpha_channel = 1; }
    if (f.channels == 4) spec.alpha_channel = 3;
    if (!f.compression.empty()) spec.attribute("compression", f.compression);
    if (f.bits) spec.attribute("oiio:BitsPerSample", f.bits);
    if (f.mixed) spec.channelformats = {TypeDesc::HALF, TypeDesc::FLOAT, TypeDesc::HALF, TypeDesc::FLOAT};
    std::vector<float> pixels(size_t(spec.width) * spec.height * spec.nchannels);
    const float rgb[] = {0.2f, 0.5f, 0.7f};
    for (size_t i = 0; i < pixels.size(); ++i) {
        int c = int(i % f.channels);
        pixels[i] = c == spec.alpha_channel ? 1.0f : f.channels <= 2 ? 0.35f : rgb[c];
    }
    write(input, spec, pixels);
    const auto original = bytes(input);
    OIIO::ImageBuf source(input.u8string());
    require(source.read(0, 0, true, TypeDesc::FLOAT), source.geterror());
    const auto& actual = source.nativespec();
    require(actual.width == 16 && actual.height == 8 && actual.nchannels == f.channels, "fixture shape changed");
    // Check on-disk native types: a silently downgraded fixture must not count as coverage.
    if (f.mixed) {
        require(actual.channelformats == spec.channelformats, "mixed channel storage types changed");
    } else {
        require(actual.format == f.type, std::string("fixture storage type changed: ") + actual.format.c_str());
    }
    if (!f.compression.empty()) require(actual.get_string_attribute("compression") == f.compression, "compression fallback");
    if (f.bits) require(actual.get_int_attribute("oiio:BitsPerSample") == f.bits, "bit depth fallback");
    float decoded[4] = {};
    source.getpixel(8, 4, decoded, 4);
    for (int c = 0; c < f.channels; ++c) {
        const float expected = c == spec.alpha_channel ? 1.0f : f.channels <= 2 ? 0.35f : rgb[c];
        require(std::abs(decoded[c] - expected) < 0.035f, "fixture decoded pixel mismatch");
    }
    thumbgen::Job job;
    job.id = name; job.input = input.u8string();
    job.options.input_space = f.linear ? "linear" : "sRGB";
    job.outputs.push_back({(root / "preview.png").u8string(), "png", 8, 4, 90});
    thumbgen::Engine engine;
    auto result = engine.convert(job);
    require(result["ok"].get<bool>(), result.dump());
    OIIO::ImageBuf output(job.outputs[0].path);
    require(output.read(), output.geterror());
    require(output.spec().width == 8 && output.spec().height == 4 && output.nchannels() == 3, "preview shape");
    float max_error = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) {
        float p[3]; output.getpixel(x, y, p, 3);
        for (int c = 0; c < 3; ++c) {
            float value = decoded[f.channels <= 2 ? 0 : c];
            const float expected = f.linear ? srgb(value) : value;
            max_error = std::max(max_error, std::abs(p[c] - expected));
        }
    }
    require(max_error < 0.012f, "preview pixel error: " + std::to_string(max_error));
    require(bytes(input) == original, "source modified");
    return {{"type", actual.format.c_str()}, {"compression", std::string(actual.get_string_attribute("compression"))},
            {"channels", actual.channelnames}, {"max_error", max_error}, {"source_unchanged", true}};
}

static Json orientation_case(const fs::path& root, int orientation) {
    const auto input = root / "orientation.tif";
    ImageSpec spec(3, 2, 3, TypeDesc::UINT8);
    spec.attribute("Orientation", orientation);
    // Six distinct colors, ordered a b c / d e f in storage order.
    const std::array<std::array<float, 3>, 6> colors = {{{1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1}}};
    std::vector<float> pixels;
    for (const auto& color : colors) pixels.insert(pixels.end(), color.begin(), color.end());
    write(input, spec, pixels);
    OIIO::ImageBuf source(input.u8string());
    require(source.init_spec(input.u8string(), 0, 0), source.geterror());
    require(source.spec().get_int_attribute("Orientation") == orientation, "fixture lost orientation");
    const int orders[8][6] = {{0,1,2,3,4,5}, {2,1,0,5,4,3}, {5,4,3,2,1,0}, {3,4,5,0,1,2},
                              {0,3,1,4,2,5}, {3,0,4,1,5,2}, {5,2,4,1,3,0}, {2,5,1,4,0,3}};
    const int width = orientation <= 4 ? 3 : 2, height = orientation <= 4 ? 2 : 3;
    thumbgen::Job job; job.id = "orientation"; job.input = input.u8string();
    job.options.input_space = "sRGB";
    job.outputs.push_back({(root / "preview.png").u8string(), "png", 10, 10, 90});
    thumbgen::Engine engine; auto result = engine.convert(job);
    require(result["ok"].get<bool>(), result.dump());
    OIIO::ImageBuf output(job.outputs[0].path); require(output.read(), output.geterror());
    require(output.spec().width == width && output.spec().height == height, "orientation dimensions");
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        float p[3]; output.getpixel(x, y, p, 3);
        auto expected = colors[orders[orientation - 1][y * width + x]];
        for (int c = 0; c < 3; ++c) require(std::abs(p[c] - expected[c]) < 0.01f, "orientation pixels");
    }
    return {{"orientation", orientation}, {"width", width}, {"height", height}};
}

static Json output_case(const fs::path& root, const std::string& format) {
    ImageSpec spec(16, 8, 4, TypeDesc::FLOAT); spec.alpha_channel = 3;
    std::vector<float> pixels(16 * 8 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 0.1f; pixels[i+1] = 0.25f; pixels[i+2] = 0.35f; pixels[i+3] = 0.5f;
    }
    const auto input = root / "alpha.exr"; write(input, spec, pixels);
    const bool transparent = format != "jpeg";
    thumbgen::Job job; job.id = format; job.input = input.u8string(); job.options.input_space = "linear";
    job.options.background = transparent ? "transparent" : "black";
    job.outputs.push_back({(root / ("preview." + (format == "jpeg" ? "jpg" : format))).u8string(), format, 8, 4, 90});
    thumbgen::Engine engine; auto result = engine.convert(job);
    require(result["ok"].get<bool>(), result.dump());
    // Read unassociated output values so the independent transfer formula applies to stored RGB.
    ImageSpec config; config.attribute("oiio:UnassociatedAlpha", 1);
    auto reader = OIIO::ImageInput::open(job.outputs[0].path, &config);
    require(bool(reader), OIIO::geterror());
    const auto actual = reader->spec();
    require(actual.width == 8 && actual.height == 4 && actual.nchannels == (transparent ? 4 : 3), "encoded shape/alpha");
    std::vector<float> decoded(size_t(8 * 4 * actual.nchannels));
    require(reader->read_image(0, 0, 0, actual.nchannels, TypeDesc::FLOAT, decoded.data()), reader->geterror());
    require(reader->close(), reader->geterror());
    const float linear[] = {0.1f, 0.25f, 0.35f};
    float max_error = 0;
    for (size_t i = 0; i < decoded.size(); i += actual.nchannels) {
        for (int c = 0; c < 3; ++c) max_error = std::max(max_error, std::abs(decoded[i+c] - srgb(linear[c] * (transparent ? 2 : 1))));
        if (transparent) require(std::abs(decoded[i+3] - 0.5f) < 0.01f, "alpha changed");
    }
    require(max_error < (format == "png" ? 0.012f : 0.045f), "encoded pixel error: " + std::to_string(max_error));
    return {{"format", format}, {"max_error", max_error}, {"alpha", transparent}};
}

static Json geometry_case(const fs::path& root, const std::string& name) {
    ImageSpec spec(2, 1, 3, TypeDesc::FLOAT);
    int width = 7, height = 5, bound = 100;
    const bool window = name.rfind("window_", 0) == 0;
    if (name == "window_negative") {
        spec.x = -2; spec.y = -2; spec.full_x = -4; spec.full_y = -3;
        spec.full_width = width; spec.full_height = height;
    } else if (name == "window_outside") {
        spec.width = 4; spec.height = 2; spec.x = -1; spec.y = 1;
        spec.full_x = 0; spec.full_y = 0; spec.full_width = width = 3; spec.full_height = height = 3;
    } else if (name == "geometry_portrait") {
        spec = ImageSpec(17, 33, 3, TypeDesc::FLOAT); width = 4; height = 8; bound = 8;
    } else if (name == "geometry_ultrawide") {
        spec = ImageSpec(128, 1, 3, TypeDesc::FLOAT); width = 8; height = 1; bound = 8;
    } else throw std::runtime_error("unknown geometry case");
    const auto input = root / "geometry.exr";
    write(input, spec, std::vector<float>(size_t(spec.width) * spec.height * 3, 1.0f));
    OIIO::ImageBuf source(input.u8string()); require(source.init_spec(input.u8string(), 0, 0), source.geterror());
    const auto& actual = source.spec();
    require(actual.x == spec.x && actual.y == spec.y && actual.full_x == spec.full_x && actual.full_y == spec.full_y
        && actual.full_width == spec.full_width && actual.full_height == spec.full_height, "fixture window changed");
    thumbgen::Job job; job.id = name; job.input = input.u8string(); job.options.input_space = "linear";
    job.outputs.push_back({(root / "preview.png").u8string(), "png", bound, bound, 90});
    thumbgen::Engine engine; auto result = engine.convert(job); require(result["ok"].get<bool>(), result.dump());
    OIIO::ImageBuf output(job.outputs[0].path); require(output.read(), output.geterror());
    require(output.spec().width == width && output.spec().height == height, "geometry dimensions");
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int sx = x + spec.full_x, sy = y + spec.full_y;
        const float expected = !window || (sx >= spec.x && sx < spec.x + spec.width && sy >= spec.y && sy < spec.y + spec.height) ? 1.0f : 0.0f;
        float p[3]; output.getpixel(x, y, p, 3);
        for (float c : p) require(std::abs(c - expected) < 0.01f, "geometry pixel placement");
    }
    return {{"width", width}, {"height", height}};
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "expected one matrix case\n"; return 2; }
    const std::string name = argv[1];
    const auto root = fs::temp_directory_path() / ("thumbgen-matrix-" + name + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    Json record = {{"case", name}, {"oiio", OIIO::get_string_attribute("version")}};
    try {
        fs::create_directory(root);
        if (name.rfind("orientation_", 0) == 0) record["checks"] = orientation_case(root, std::stoi(name.substr(12)));
        else if (name.rfind("output_", 0) == 0) record["checks"] = output_case(root, name.substr(7));
        else if (name.rfind("window_", 0) == 0 || name.rfind("geometry_", 0) == 0) record["checks"] = geometry_case(root, name);
        else record["checks"] = format_case(root, name);
        record["ok"] = true; std::cout << record.dump() << std::endl; return 0;
    } catch (const std::exception& e) {
        record["ok"] = false; record["error"] = e.what(); std::cerr << record.dump() << std::endl; return 1;
    }
}
