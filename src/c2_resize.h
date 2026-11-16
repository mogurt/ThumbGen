#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
namespace thumbgen::detail {
struct C2PlanLimit:std::runtime_error {C2PlanLimit():std::runtime_error("C2 workspace limit"){} };
struct C2ContractError:std::runtime_error {using std::runtime_error::runtime_error;};
// Internal opt-in adapter. Construction fixes all geometry and allocates the
// original sampler tables plus bounded concurrent slice state, before decode.
class C2Resize {
public:
    // Geometry-only counting pass: no pixel access or dynamic allocations.
    // False means structural fallback; call before admission, construct after it.
    static bool workspace_plan(int width,int height,int channels,const std::vector<std::pair<int,int>>& outputs,std::size_t& bytes);
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
