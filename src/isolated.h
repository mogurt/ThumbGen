#pragma once
#include <thumbgen/core.h>
#include <chrono>

namespace thumbgen::detail {
struct IsolationOptions {
    // Zero preserves the existing per-buffer limits without a scheduling cap.
    size_t memory_mib=0;
    unsigned thread_budget=0; // Zero infers the largest requested job quota.
    unsigned workers=1;
    std::chrono::milliseconds startup_timeout{30000},task_timeout{0};
};
Json execution_configuration(const std::vector<Job>& jobs,const IsolationOptions& options);
// Internal server entry, also used by dedicated fault-injection test workers.
int serve_worker(const std::string& token,const std::string& worker,unsigned threads,Engine::Observer observer={});
// Bounded process-isolated execution. Caller has already preflighted all jobs.
// Results are emitted once each; native faults do not prevent later jobs.
void run_isolated(const std::vector<Job>& jobs,const std::string& executable,
                  const std::function<void(const Json&)>& emit,IsolationOptions options={});
}
