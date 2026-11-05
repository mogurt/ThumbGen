#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace thumbgen::detail {
struct DeepSample {
    float z=0,zback=0;
    std::array<float,3> colour{},alpha{};
};
struct FlatDeepPixel {
    std::array<double,3> colour{},alpha{};
};
struct DeepLimits {
    std::size_t samples_per_pixel=4096;
    std::size_t workspace_bytes=8*1024*1024;
};
class DeepFailure : public std::runtime_error {
public:
    const char* code;
    DeepFailure(const char* c,const char* message):std::runtime_error(message),code(c){}
};
// Pure selected-colour math, not a file decoder or public SDK. Colour is
// associated independently with each RGB alpha. HDR colour is not clamped;
// this layer does not apply OCIO or interpret ID data.
class DeepFlattener {
public:
    explicit DeepFlattener(DeepLimits limits={});
    FlatDeepPixel flatten(const DeepSample* samples,std::size_t count);
    std::size_t retained_workspace_bytes() const noexcept;
    static std::size_t workspace_bound(std::size_t count);
private:
    struct Component {double density=0,emission=0,opaque_colour=0;std::uint32_t opaque_count=0;};
    struct Node {std::array<Component,3> channel;};
    enum class Kind {end,point,begin};
    struct Event {float depth;Kind kind;std::size_t sample;};
    static Node join(const Node& a,const Node& b);
    static Node contribution(const DeepSample& sample,bool volume);
    static void composite(FlatDeepPixel& result,std::array<double,3>& transmission,const Node& node,double extent);
    DeepLimits limits_;
    std::vector<DeepSample> samples_;
    std::vector<Event> events_;
    std::vector<Node> tree_;
};
}
