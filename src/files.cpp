#include "files.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <random>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace thumbgen::detail {
namespace fs=std::filesystem;
std::string error_message(const std::error_code& error) {
    auto message=error.message();
#ifdef _WIN32
    // The Windows standard library may return localized messages in the ANSI
    // code page, while the public JSON contract always requires UTF-8.
    int count=MultiByteToWideChar(CP_ACP,0,message.data(),int(message.size()),nullptr,0);
    if(count>0) {
        std::wstring wide(size_t(count),L'\0');MultiByteToWideChar(CP_ACP,0,message.data(),int(message.size()),wide.data(),count);
        int size=WideCharToMultiByte(CP_UTF8,0,wide.data(),count,nullptr,0,nullptr,nullptr);
        if(size>0){std::string text(size_t(size),'\0');WideCharToMultiByte(CP_UTF8,0,wide.data(),count,text.data(),size,nullptr,nullptr);return text;}
    }
#endif
    return message;
}
std::string file_identity(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION info{};bool ok=GetFileInformationByHandle(handle,&info)!=0;CloseHandle(handle);
    if(!ok)return {};
    return std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
#else
    struct stat info{};if(::stat(path.c_str(),&info)!=0)return {};
    return std::to_string(info.st_dev)+":"+std::to_string(info.st_ino);
#endif
}
bool existing_target(const fs::path& path,const fs::path& source) {
    std::error_code ec;auto status=fs::symlink_status(path,ec);
    if(status.type()==fs::file_type::not_found || ec==std::errc::no_such_file_or_directory)return false;
    if(ec)throw FileFailure("output_stat_failed",error_message(ec));
    if(fs::is_symlink(status))throw FileFailure("output_symlink","output leaf must not be a symbolic link");
    if(!fs::is_regular_file(status))throw FileFailure("output_not_regular","existing output is not a regular file");
    if(fs::equivalent(path,source,ec) && !ec)throw FileFailure("output_aliases_input","output aliases the input file");
    return true;
}
TemporaryOutput::TemporaryOutput(const fs::path& target,const std::string& format) {
    fs::create_directories(target.parent_path());
    static std::atomic<unsigned long long> serial{0};
    std::random_device random;
    for(int attempt=0;attempt<16;++attempt) {
        auto candidate=target.parent_path()/fs::u8path(".thumbgen-tmp-"+std::to_string(random())+"-"+std::to_string(++serial));
        if(fs::create_directory(candidate)) {
            directory_=std::move(candidate);file_=directory_/fs::u8path("image."+format);return;
        }
    }
    throw FileFailure("temporary_create_failed","cannot reserve an exclusive temporary directory");
}
TemporaryOutput::~TemporaryOutput(){cleanup();}
std::string TemporaryOutput::cleanup() noexcept {
    std::error_code file_error,dir_error;
    if(!file_.empty())fs::remove(file_,file_error);
    if(!directory_.empty())fs::remove(directory_,dir_error);
    // Never recurse: only the file and directory reserved by this object belong to us.
    return file_error?error_message(file_error):dir_error?error_message(dir_error):std::string{};
}
bool TemporaryOutput::commit(const fs::path& target,const fs::path& source,const std::string& policy) {
    const bool exists=existing_target(target,source);
    if(exists && policy=="skip")return false;
    if(exists && policy=="error")throw FileFailure("output_exists","output already exists");
#ifdef _WIN32
    DWORD flags=MOVEFILE_WRITE_THROUGH|(policy=="replace"?MOVEFILE_REPLACE_EXISTING:0);
    if(MoveFileExW(file_.c_str(),target.c_str(),flags))return true;
    auto error=GetLastError();
    if(error==ERROR_FILE_EXISTS || error==ERROR_ALREADY_EXISTS) {
#else
    if(policy=="replace") {
        if(::rename(file_.c_str(),target.c_str())==0)return true;
    } else if(::link(file_.c_str(),target.c_str())==0)return true;
    auto error=errno;
    if(error==EEXIST) {
#endif
        if(policy=="skip")return false;
        throw FileFailure("output_exists","output appeared before commit");
    }
    throw FileFailure("commit_failed",error_message(std::error_code(int(error),std::system_category())));
}
SourceStamp source_stamp(const fs::path& path) {
    // Query identity, size and time together instead of three metadata round
    // trips. Native timestamps are compared only within the same process/OS.
#ifdef _WIN32
    HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw FileFailure("source_stat_failed",error_message(std::error_code(int(GetLastError()),std::system_category())));
    BY_HANDLE_FILE_INFORMATION info{};bool ok=GetFileInformationByHandle(handle,&info)!=0;
    auto error=GetLastError();CloseHandle(handle);
    if(!ok)throw FileFailure("source_stat_failed",error_message(std::error_code(int(error),std::system_category())));
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)throw FileFailure("source_stat_failed","input is not a regular file");
    auto identity=std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
    auto size=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    auto modified=(std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    return {identity,size,static_cast<std::int64_t>(modified),0};
#else
    struct stat info{};
    if(::stat(path.c_str(),&info)!=0)throw FileFailure("source_stat_failed",error_message(std::error_code(errno,std::system_category())));
    if(!S_ISREG(info.st_mode))throw FileFailure("source_stat_failed","input is not a regular file");
    return {std::to_string(info.st_dev)+":"+std::to_string(info.st_ino),static_cast<std::uintmax_t>(info.st_size),
        static_cast<std::int64_t>(info.st_mtim.tv_sec),static_cast<std::int64_t>(info.st_mtim.tv_nsec)};
#endif
}
bool source_matches(const fs::path& path,const SourceStamp& stamp) noexcept {
    try {
        auto now=source_stamp(path);
        return now.identity==stamp.identity && now.size==stamp.size && now.modified_seconds==stamp.modified_seconds && now.modified_fraction==stamp.modified_fraction;
    }catch(...){return false;}
}
}
