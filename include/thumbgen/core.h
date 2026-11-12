#pragma once
#include <nlohmann/json.hpp>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace thumbgen {
using Json = nlohmann::json;
struct Options {
    std::string preset = "balanced", input_space = "auto", background = "black";
    std::string ocio_config, display, view, channels, part_name, selection_policy = "primary";
    std::string visualization="colour",invalid_values="magenta";
    std::string deep_mode="reject";
    int deep_samples_per_pixel=4096;
    std::vector<double> data_range;
    int part = -1, threads = 4;
    size_t max_decode_mib = 512;
};
struct Output {
    std::string path, format = "jpeg";
    int width = 1024, height = 1024, quality = 90;
    std::string existing = "error";
};
struct Job {
    std::string id, input, sequence_id;
    Options options;
    std::vector<Output> outputs;
};
// Internal interface, not a stable public C++ ABI.
class Engine {
public:
    // Internal phase observer; used by native integration tests and future scheduling.
    using Observer = std::function<void(const std::string&,const Job&)>;
    struct Control {
        // Called after metadata selection, before colour/pixel allocation.
        std::function<void(const Job&,const Json&)> planned;
        // The controller owns the returned absolute, nonexistent temporary path.
        std::function<std::string(const Job&,size_t)> reserve;
        // Called only after the encoded stream is closed. False means skip.
        std::function<bool(const Job&,size_t,const std::string&,const Json&)> commit;
        // Internal reference switch for paired processing tests, not a CLI option.
        bool reference_colour=false;
    };
    explicit Engine(Observer observer = {});
    ~Engine();
    Json inspect(const std::string& input);
    Json convert(const Job& job);
    // Set/import only while idle. reserve and commit must be supplied together.
    void set_control(Control control);
    Json sequence_lock(const std::string& sequence_id) const;
    void restore_sequence_lock(const std::string& sequence_id,const Json& lock);
    void forget_sequence_lock(const std::string& sequence_id);
    static Json capabilities();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
