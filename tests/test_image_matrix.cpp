#include <thumbgen/core.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
    const bool rotated = name == "window_negative_rotated";
    if (name == "window_negative" || rotated) {
        spec.x = -2; spec.y = -2; spec.full_x = -4; spec.full_y = -3;
        spec.full_width = width; spec.full_height = height;
        if (rotated) { spec.attribute("Orientation", 6); width = 5; height = 7; }
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
    if (rotated) require(actual.get_int_attribute("Orientation", 1) == 6, "fixture lost window orientation");
    require(actual.x == spec.x && actual.y == spec.y && actual.full_x == spec.full_x && actual.full_y == spec.full_y
        && actual.full_width == spec.full_width && actual.full_height == spec.full_height, "fixture window changed");
    thumbgen::Job job; job.id = name; job.input = input.u8string(); job.options.input_space = "linear";
    job.outputs.push_back({(root / "preview.png").u8string(), "png", bound, bound, 90});
    thumbgen::Engine engine; auto result = engine.convert(job); require(result["ok"].get<bool>(), result.dump());
    OIIO::ImageBuf output(job.outputs[0].path); require(output.read(), output.geterror());
    require(output.spec().width == width && output.spec().height == height, "geometry dimensions");
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int sx = (rotated ? y : x) + spec.full_x;
        const int sy = (rotated ? spec.full_height - 1 - x : y) + spec.full_y;
        const float expected = !window || (sx >= spec.x && sx < spec.x + spec.width && sy >= spec.y && sy < spec.y + spec.height) ? 1.0f : 0.0f;
        float p[3]; output.getpixel(x, y, p, 3);
        for (float c : p) require(std::abs(c - expected) < 0.01f, "geometry pixel placement at " + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(c) + " expected " + std::to_string(expected));
    }
    return {{"width", width}, {"height", height}};
}

struct OcioEnvironment {
    std::string previous;
    bool present;
    OcioEnvironment() : present(std::getenv("OCIO") != nullptr) {
        if (present) previous = std::getenv("OCIO");
        set("");
    }
    static void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s("OCIO", value.c_str());
#else
        if (value.empty()) unsetenv("OCIO"); else setenv("OCIO", value.c_str(), 1);
#endif
    }
    ~OcioEnvironment() { set(present ? previous : ""); }
};
static Json colour_case(const fs::path& root, const std::string& name) {
    OcioEnvironment environment;
    const auto config_path = root / "test.ocio";
    {
        std::ofstream file(config_path);
        file << "ocio_profile_version: 2\nsearch_path: ''\nstrictparsing: true\nroles:\n  default: workingRGB\n";
        if (name != "colour_missing_role") file << "  scene_linear: " << (name == "colour_nonlinear_role" ? "encodedInput" : "workingRGB") << "\n";
        file << R"(file_rules:
  - !<Rule> {name: Default, colorspace: workingRGB}
displays:
  testDisplay:
    - !<View> {name: testView, colorspace: sRGB}
colorspaces:
  - !<ColorSpace>
    name: workingRGB
    bitdepth: 32f
    isdata: false
    allocation: uniform
  - !<ColorSpace>
    name: encodedInput
    bitdepth: 32f
    isdata: false
    to_scene_reference: !<ExponentTransform> {value: [2, 2, 2, 1]}
  - !<ColorSpace>
    name: sRGB
    bitdepth: 32f
    isdata: false
    from_scene_reference: !<ExponentTransform> {value: [0.5, 0.5, 0.5, 1]}
)";
    }
    const bool metadata = name == "colour_metadata" || name == "colour_override";
    ImageSpec spec(2, 2, 3, metadata ? TypeDesc::UINT16 : TypeDesc::FLOAT);
    spec.attribute("oiio:ColorSpace", "sRGB");
    const auto input = root / (metadata ? "input.png" : "input.exr");
    write(input, spec, std::vector<float>(12, 0.5f));
    thumbgen::Job job; job.id = name; job.input = input.u8string();
    job.options.ocio_config = config_path.u8string(); job.options.input_space = "encodedInput";
    job.outputs.push_back({(root / "preview.png").u8string(), "png", 2, 2, 90});
    if (name == "colour_environment") {
        environment.set(config_path.u8string()); job.options.ocio_config.clear();
    } else if (name == "colour_invalid_environment") {
        environment.set((root / "missing-env.ocio").u8string()); job.options.ocio_config.clear();
    } else if (name == "colour_explicit_priority") environment.set((root / "missing-env.ocio").u8string());
    else if (name == "colour_metadata") job.options.input_space = "auto";
    else if (name == "colour_override") job.options.input_space = "workingRGB";
    else if (name == "colour_display" || name == "colour_missing_display" || name == "colour_missing_view") {
        job.options.display = name == "colour_missing_display" ? "missing" : "testDisplay";
        job.options.view = name == "colour_missing_view" ? "missing" : "testView";
    } else if (name == "colour_missing_space") job.options.input_space = "missing";
    else if (name == "colour_invalid_repeat") job.options.ocio_config = (root / "missing.ocio").u8string();
    else if (name == "colour_builtin") { job.options.ocio_config.clear(); job.options.input_space = "linear"; }
    std::string error;
    if (name == "colour_missing_role") error = "colour_working_space_missing";
    if (name == "colour_nonlinear_role") error = "colour_working_space_invalid";
    if (name == "colour_missing_space") error = "colour_space_not_found";
    if (name == "colour_missing_display") error = "colour_display_not_found";
    if (name == "colour_missing_view") error = "colour_view_not_found";
    if (name == "colour_invalid_repeat" || name == "colour_invalid_environment") error = "colour_config_failed";
    thumbgen::Engine engine;
    auto result = engine.convert(job);
    if (!error.empty()) {
        for (int repeat = 0; repeat < 2; ++repeat) {
            require(!result["ok"].get<bool>() && result["error"]["code"] == error, result.dump());
            require(!fs::exists(root / "preview.png"), "invalid colour request wrote output");
            result = engine.convert(job);
        }
        return {{"expected_error", error}};
    }
    require(result["ok"].get<bool>(), result.dump());
    const auto& colour = result["colour"];
    require(colour["config_basis"] == (name == "colour_environment" ? "environment" : name == "colour_builtin" ? "builtin" : "explicit"), result.dump());
    if (name != "colour_builtin") require(colour["working_space"] == "workingRGB", result.dump());
    if (name == "colour_metadata") require(colour["basis"] == "metadata", result.dump());
    OIIO::ImageBuf output(job.outputs[0].path); require(output.read(), output.geterror());
    float p[3]; output.getpixel(0, 0, p, 3);
    const float expected = name == "colour_builtin" ? srgb(0.5f) : name == "colour_override" ? std::sqrt(0.5f) : 0.5f;
    for (float c : p) require(std::abs(c - expected) < 0.012f, "custom colour transform value");
    return colour;
}
static float linear(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}
static Json alpha_case(const fs::path& root, const std::string& name) {
    OcioEnvironment environment;
    const bool edge = name == "alpha_edge";
    const bool zero = name == "alpha_zero";
    const bool associated = name == "alpha_exr_associated";
    const std::string extension = associated ? "exr" : name == "alpha_webp" ? "webp" : name == "alpha_tiff" ? "tif" : "png";
    const auto input = root / ("input." + extension);
    ImageSpec spec(2, 2, 4, associated ? TypeDesc::FLOAT : TypeDesc::UINT16); spec.alpha_channel = 3;
    spec.attribute("oiio:UnassociatedAlpha", associated ? 0 : 1);
    spec.attribute("CompressionQuality", 100);
    if (extension == "webp") spec.attribute("compression", "lossless");
    const float rgb[3] = {0.2f, 0.5f, 0.7f};
    std::vector<float> pixels(16);
    for (int i = 0; i < 4; ++i) {
        float a = zero ? 0.0f : edge ? (i % 2 == 0 ? 1.0f : 0.0f) : 0.5f;
        for (int c = 0; c < 3; ++c) pixels[i*4+c] = edge ? (c == (a == 1 ? 0 : 2) ? 1.0f : 0.0f) : associated ? linear(rgb[c]) * a : rgb[c];
        pixels[i*4+3] = a;
    }
    write(input, spec, pixels);
    const auto original = bytes(input);
    thumbgen::Job job; job.id = name; job.input = input.u8string(); job.options.input_space = associated ? "linear" : "sRGB";
    for (const char* background : {"black", "white", "transparent"}) {
        job.options.background = background;
        job.outputs = {{(root / (std::string(background) + ".png")).u8string(), "png", edge ? 1 : 2, edge ? 1 : 2, 90}};
        thumbgen::Engine engine; auto result = engine.convert(job); require(result["ok"].get<bool>(), result.dump());
        ImageSpec config; config.attribute("oiio:UnassociatedAlpha", 1);
        auto reader = OIIO::ImageInput::open(job.outputs[0].path, &config); require(bool(reader), OIIO::geterror());
        const int channel_count = reader->spec().nchannels;
        std::vector<float> p(size_t(reader->spec().width) * reader->spec().height * channel_count);
        require(reader->read_image(0, 0, 0, channel_count, TypeDesc::FLOAT, p.data()), reader->geterror());
        reader->close();
        const bool transparent = std::string(background) == "transparent";
        for (int c = 0; c < 3; ++c) {
            float value = edge ? (c == 0 ? 1.0f : 0.0f) : linear(rgb[c]);
            const float a = zero ? 0.0f : 0.5f;
            const float expected = srgb(transparent ? (zero ? 0 : value) : value * a + (std::string(background) == "white" ? 1-a : 0));
            require(std::abs(p[c] - expected) < 0.018f, name + " " + background + " RGB mismatch " + std::to_string(p[c]) + " expected " + std::to_string(expected));
        }
        if (transparent) require(channel_count == 4 && std::abs(p[3] - (zero ? 0.0f : 0.5f)) < 0.01f, "alpha not preserved");
    }
    require(bytes(input) == original, "alpha source modified");
    return {{"backgrounds", {"black", "white", "transparent"}}, {"source_unchanged", true}};
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
        else if (name.rfind("colour_", 0) == 0) record["checks"] = colour_case(root, name);
        else if (name.rfind("alpha_", 0) == 0) record["checks"] = alpha_case(root, name);
        else if (name.rfind("output_", 0) == 0) record["checks"] = output_case(root, name.substr(7));
        else if (name.rfind("window_", 0) == 0 || name.rfind("geometry_", 0) == 0) record["checks"] = geometry_case(root, name);
        else record["checks"] = format_case(root, name);
        record["ok"] = true; std::cout << record.dump() << std::endl; return 0;
    } catch (const std::exception& e) {
        record["ok"] = false; record["error"] = e.what(); std::cerr << record.dump() << std::endl; return 1;
    }
}
