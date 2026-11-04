#pragma once
#include <thumbgen/core.h>
#include <cstdint>
#include <stdexcept>

namespace thumbgen::detail {
struct ProtocolError:std::runtime_error {using std::runtime_error::runtime_error;};
struct WorkerMessage {std::string type,task_id;Json payload;};
class WorkerSession {
public:
    static constexpr size_t max_frame=16*1024*1024;
    WorkerSession(std::string token,std::string worker);
    std::string encode(const std::string& type,const std::string& task,const Json& payload);
    WorkerMessage decode(const std::string& frame);
    [[noreturn]] void reject(const std::string& reason);
    bool failed() const {return failed_;}
private:
    std::string token_,worker_;
    std::uint64_t sent_=0,received_=0;
    bool failed_=false;
};
Json job_to_wire(const Job& job);
Job job_from_wire(const Json& value);
}
