#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace thumbgen::detail {
// Internal scheduling model, not a public SDK. All sizes are bytes.
struct ImageFootprint {
    std::uint64_t data_width=0,data_height=0,display_width=0,display_height=0;
    std::uint64_t channel_span=0,selected_channels=0,output_width=0,output_height=0;
    unsigned threads=0;
};
struct MemoryEstimate {
    std::uint64_t source=0,working=0,output=0,codec=0,worker=0,margin=0,total=0;
};
// Conservative admission estimate, never an OS-enforced RSS bound.
// Throws invalid_argument for invalid geometry; overflow_error for arithmetic overflow.
MemoryEstimate estimate_memory(const ImageFootprint& shape);

struct BudgetLimits {
    unsigned parallel=1,threads=4;
    std::uint64_t memory=0;
    // Parent/IPC and retained idle workers remain charged between tasks.
    std::uint64_t reserved_memory=0;
};
struct ScheduledTask {
    std::string id;
    unsigned requested_threads=4;
    std::uint64_t estimated_memory=0;
};
struct Admission {
    enum class State {empty,waiting,admitted,rejected};
    State state=State::empty;
    ScheduledTask task;
    unsigned threads=0;
    std::string reason;
};
class BudgetQueue {
public:
    explicit BudgetQueue(BudgetLimits limits);
    unsigned threads_for(unsigned requested) const;
    void push(ScheduledTask task);
    // Remove a prepared task whose worker failed before admission.
    // Its id stays seen, so it cannot be accidentally scheduled a second time.
    bool erase_pending(const std::string& id);
    // Strict FIFO: an oversized head is rejected, a temporarily blocked head waits.
    Admission take();
    // Only an admitted task may release resources, exactly once.
    void release(const std::string& id);
    std::size_t pending() const {return pending_.size();}
    std::size_t active() const {return active_.size();}
    std::uint64_t memory_in_use() const {return memory_;}
    unsigned threads_in_use() const {return threads_;}
private:
    BudgetLimits limits_;
    std::deque<ScheduledTask> pending_;
    std::map<std::string,Admission> active_;
    std::set<std::string> seen_;
    std::uint64_t memory_=0;
    unsigned threads_=0;
};
}
