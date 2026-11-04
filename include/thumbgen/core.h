#pragma once
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace thumbgen {
using Json = nlohmann::json;
struct Options {
    std::string preset = "balanced", input_space = "auto", background = "black";
    std::string ocio_config, display, view, channels;
    int part = -1, threads = 4;
    size_t max_decode_mib = 512;
};
struct Output {
    std::string path, format = "jpeg";
    int width = 1024, height = 1024, quality = 90;
};
struct Job {
    std::string id, input;
    Options options;
    std::vector<Output> outputs;
};
// Internal interface, not a stable public C++ ABI.
class Engine {
public:
    Engine();
    ~Engine();
    Json inspect(const std::string& input);
    Json convert(const Job& job);
    static Json capabilities();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
