#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
namespace thumbgen::detail {
struct C2PlanLimit:std::runtime_error {C2PlanLimit():std::runtime_error("C2 workspace limit"){} };
// Internal opt-in adapter. Construction fixes all geometry and allocates the
// original sampler tables plus bounded concurrent slice state, before decode.
class C2Resize {
public:
    C2Resize(int width,int height,int channels,const std::vector<std::pair<int,int>>& outputs);
    ~C2Resize();
    C2Resize(const C2Resize&)=delete;
    C2Resize& operator=(const C2Resize&)=delete;
    std::size_t workspace_bytes() const;
    // read_row fills a stable row. Exceptions abort; no retries or redecoding.
    std::vector<double> execute(const std::function<void(int,float*)>& read_row,const std::vector<float*>& outputs);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
