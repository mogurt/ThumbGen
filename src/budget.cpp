#include "budget.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace thumbgen::detail {
namespace {
using Bytes=std::uint64_t;
constexpr Bytes mib=1024*1024;
Bytes add(Bytes a,Bytes b) {
    if(b>std::numeric_limits<Bytes>::max()-a)throw std::overflow_error("memory estimate addition overflow");
    return a+b;
}
Bytes multiply(Bytes a,Bytes b) {
    if(a && b>std::numeric_limits<Bytes>::max()/a)throw std::overflow_error("memory estimate multiplication overflow");
    return a*b;
}
}
MemoryEstimate estimate_memory(const ImageFootprint& s) {
    if(!s.data_width || !s.data_height || !s.display_width || !s.display_height || !s.channel_span ||
       (s.selected_channels!=3 && s.selected_channels!=4) || !s.output_width || !s.output_height ||
       !s.threads || s.threads>256 || !s.source_sample_bytes || s.source_sample_bytes>8 ||
       (s.native_categorical && s.selected_channels!=4))throw std::invalid_argument("invalid memory estimate shape");
    const Bytes data=multiply(s.data_width,s.data_height);
    const Bytes canvas=multiply(s.display_width,s.display_height);
    const Bytes preview=multiply(s.output_width,s.output_height);
    if(preview>canvas)throw std::invalid_argument("preview footprint exceeds display canvas");
    MemoryEstimate e;
    e.source=multiply(multiply(data,s.channel_span),s.source_sample_bytes);
    // Categorical modes retain native source + per-thread DOUBLE scan rows;
    // RGBA is allocated at preview size and already covered by output below.
    // Other modes keep full colour/mapping, padding and orientation temporaries.
    e.working=s.native_categorical?multiply(multiply(s.data_width,s.threads),3*sizeof(double)):
        multiply(multiply(std::max(data,canvas),s.selected_channels),3*sizeof(float));
    // Resize/composite/unpremult/RGB conversion can have overlapping buffers.
    // This allowance also covers the final UINT8 RGB(A) buffer (<=4 bytes/pixel)
    // while its FLOAT source remains alive; it replaces codec float conversion.
    e.output=multiply(preview,5*4*sizeof(float));
    // Includes JPEG's compressed memory destination; not a codec allocation bound.
    e.codec=add(multiply(preview,8),mib);
    // Provisional measured-headroom model, to calibrate across codecs and platforms.
    e.worker=add(128*mib,multiply(s.threads,8*mib));
    Bytes dynamic=add(add(e.source,e.working),add(e.output,e.codec));
    e.margin=dynamic/4+(dynamic%4!=0); // ceil(25%), without overflow-prone +3
    e.incremental=add(dynamic,e.margin);
    e.total=add(e.incremental,e.worker);
    return e;
}
BudgetQueue::BudgetQueue(BudgetLimits limits):limits_(limits),memory_(limits.reserved_memory) {
    if(!limits.parallel || limits.parallel>256 || !limits.threads || limits.threads>256 || !limits.memory)
        throw std::invalid_argument("budget limits must be positive; parallel and threads at most 256");
    if(limits.reserved_memory>limits.memory)throw std::invalid_argument("fixed memory reserve exceeds total budget");
    limits_.parallel=std::min(limits.parallel,limits.threads);
}
unsigned BudgetQueue::threads_for(unsigned requested) const {
    if(!requested || requested>256)throw std::invalid_argument("requested threads must be in 1..256");
    return std::min(requested,std::max(1u,limits_.threads/limits_.parallel));
}
void BudgetQueue::push(ScheduledTask task) {
    if(task.id.empty() || !task.estimated_memory)throw std::invalid_argument("task id and memory estimate are required");
    threads_for(task.requested_threads);
    if(!seen_.insert(task.id).second)throw std::invalid_argument("duplicate scheduled task id");
    pending_.push_back(std::move(task));
}
Admission BudgetQueue::take() {
    if(pending_.empty())return {};
    Admission result;result.task=pending_.front();result.threads=threads_for(result.task.requested_threads);
    if(result.task.estimated_memory>limits_.memory-limits_.reserved_memory) {
        result.state=Admission::State::rejected;result.reason="task_memory_exceeds_budget";
        pending_.pop_front();return result;
    }
    if(active_.size()>=limits_.parallel || result.threads>limits_.threads-threads_ ||
       result.task.estimated_memory>limits_.memory-memory_) {
        result.state=Admission::State::waiting;return result;
    }
    result.state=Admission::State::admitted;
    active_.emplace(result.task.id,result);
    memory_+=result.task.estimated_memory;threads_+=result.threads;
    pending_.pop_front();return result;
}
bool BudgetQueue::erase_pending(const std::string& id) {
    auto item=std::find_if(pending_.begin(),pending_.end(),[&](const ScheduledTask& task){return task.id==id;});
    if(item==pending_.end())return false;
    pending_.erase(item);return true;
}
void BudgetQueue::release(const std::string& id) {
    auto item=active_.find(id);
    if(item==active_.end())throw std::invalid_argument("task has no active reservation");
    memory_-=item->second.task.estimated_memory;threads_-=item->second.threads;
    active_.erase(item);
}
}
