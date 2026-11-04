#pragma once
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace thumbgen::detail {
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
    ChildProcess(const std::string& executable,const std::vector<std::string>& arguments,PipeLimits limits={});
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
