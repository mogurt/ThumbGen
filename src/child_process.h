#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace thumbgen::detail {
// Shared by a controller and its child I/O pumps. Capture sequence BEFORE
// scanning child queues, then wait on that sequence to avoid lost wakeups.
// Never hold this mutex while inspecting a ChildProcess queue.
class ProcessWakeup {
public:
    std::uint64_t sequence() const;
    void notify() noexcept;
    bool wait(std::uint64_t observed,std::chrono::steady_clock::time_point deadline);
#if defined(THUMBGEN_TEST_HOOKS)
    // Test-only: raise the condition variable without advancing the sequence,
    // so a predicated wait must keep waiting. Defined only in test builds.
    void spurious() noexcept;
#endif
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t sequence_=0;
};
struct PipeLimits {
    std::size_t line_bytes=16*1024*1024;
    std::size_t queued_bytes=32*1024*1024;
    std::size_t queued_lines=64;
    std::size_t stderr_bytes=64*1024;
};
struct PipeLine {
    enum class State {line,timeout,eof,fault};
    State state=State::eof;
    std::string text;
};
// Single controlling thread; I/O pumps are internal. No shell or PATH search.
// stop/destruction terminates only this instance's process group/job and reaps
// the direct child. Native code that deliberately escapes a POSIX process group
// is outside this ownership boundary.
// POSIX callers must not reap this child in another thread/signal handler.
class ChildProcess {
public:
    ChildProcess(const std::string& executable,const std::vector<std::string>& arguments,PipeLimits limits={},std::shared_ptr<ProcessWakeup> wakeup={});
    ~ChildProcess();
    ChildProcess(const ChildProcess&)=delete;
    ChildProcess& operator=(const ChildProcess&)=delete;
    // Queue a newline-delimited frame without blocking on the child's stdin.
    // Queue/frame overflow throws; callers must stop the associated worker.
    void send(std::string line);
    PipeLine read(std::chrono::steady_clock::time_point deadline);
    std::optional<int> exit_code();
    std::string stderr_text() const;
    bool stderr_truncated() const;
    std::string pipe_fault() const;
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
