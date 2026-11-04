#pragma once
#include <filesystem>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace thumbgen::detail {
struct FileFailure : std::runtime_error {
    std::string code;
    bool retryable;
    FileFailure(std::string c,std::string message,bool retry=false)
        :std::runtime_error(std::move(message)),code(std::move(c)),retryable(retry){}
};
// Empty when metadata cannot be queried (e.g. a missing input).
std::string file_identity(const std::filesystem::path& path);
std::string error_message(const std::error_code& error);
bool existing_target(const std::filesystem::path& path,const std::filesystem::path& source);
class TemporaryOutput {
public:
    TemporaryOutput(const std::filesystem::path& target,const std::string& format);
    ~TemporaryOutput();
    TemporaryOutput(const TemporaryOutput&)=delete;
    TemporaryOutput& operator=(const TemporaryOutput&)=delete;
    const std::filesystem::path& path() const {return file_;}
    // False means another writer won a skip-policy race; no overwrite occurred.
    bool commit(const std::filesystem::path& target,const std::filesystem::path& source,const std::string& policy);
    std::string cleanup() noexcept;
private:
    std::filesystem::path directory_,file_;
};
struct SourceStamp {
    std::string identity;
    std::uintmax_t size;
    std::int64_t modified_seconds,modified_fraction;
};
SourceStamp source_stamp(const std::filesystem::path& path);
bool source_matches(const std::filesystem::path& path,const SourceStamp& stamp) noexcept;
}
