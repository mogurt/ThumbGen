#pragma once
#include <thumbgen/core.h>
#include <chrono>
#if defined(THUMBGEN_TEST_HOOKS)
#include <functional>
#include <memory>
#include "child_process.h"
#endif

namespace thumbgen::detail {
struct IsolationOptions {
    // Zero preserves the existing per-buffer limits without a scheduling cap.
    size_t memory_mib=0;
    unsigned thread_budget=0; // Zero infers the largest requested job quota.
    unsigned workers=1;
    std::chrono::milliseconds startup_timeout{30000},task_timeout{0};
    // Invoked only by the controller thread; once true, cancellation is latched.
    std::function<bool()> cancelled;
};
Json execution_configuration(const std::vector<Job>& jobs,const IsolationOptions& options);
#if defined(THUMBGEN_TEST_HOOKS)
// Test-only controller scan hook (defined only when THUMBGEN_TEST_HOOKS is
// set for the runtime translation unit): the controller thread invokes it at
// the no-progress scan/wait boundary of run_isolated with its shared wakeup,
// so tests can interleave a notification exactly there. Reset after use.
void set_test_scan_hook(std::function<void(const std::shared_ptr<ProcessWakeup>&)> hook={});
#endif
// Internal server entry, also used by dedicated fault-injection test workers.
int serve_worker(const std::string& token,const std::string& worker,unsigned threads,Engine::Observer observer={});
// Bounded process-isolated execution. Caller has already preflighted all jobs.
// Results are emitted once each; native faults do not prevent later jobs.
bool run_isolated(const std::vector<Job>& jobs,const std::string& executable,
                  const std::function<void(const Json&)>& emit,IsolationOptions options={});
}
