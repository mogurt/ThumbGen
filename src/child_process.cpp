#include "child_process.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace thumbgen::detail {
std::uint64_t ProcessWakeup::sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);return sequence_;
}
void ProcessWakeup::notify() noexcept {
    {std::lock_guard<std::mutex> lock(mutex_);++sequence_;}
    changed_.notify_all();
}
bool ProcessWakeup::wait(std::uint64_t observed,std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready=[&]{return sequence_!=observed;};
    if(deadline==std::chrono::steady_clock::time_point::max()){changed_.wait(lock,ready);return true;}
    return changed_.wait_until(lock,deadline,ready);
}
#if defined(THUMBGEN_TEST_HOOKS)
void ProcessWakeup::spurious() noexcept {changed_.notify_all();}
#endif
namespace {
#ifdef _WIN32
struct NativeHandle {
    HANDLE value=nullptr;
    NativeHandle()=default;
    NativeHandle(const NativeHandle&)=delete;
    ~NativeHandle(){reset();}
    void reset(HANDLE next=nullptr){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=next;}
    HANDLE release(){auto result=value;value=nullptr;return result;}
};
void native_check(bool ok,const char* what) {
    if(!ok)throw std::system_error(int(GetLastError()),std::system_category(),what);
}
std::wstring wide(const std::string& text) {
    if(text.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(!n)throw std::invalid_argument("process argument is not UTF-8");
    std::wstring result(std::size_t(n),L'\0');
    native_check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),n)!=0,"UTF-8 argument");
    return result;
}
std::wstring quote(const std::wstring& arg) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(auto c:arg) {
        if(c==L'\\'){++slashes;continue;}
        result.append(c==L'"'?2*slashes+1:slashes,L'\\');slashes=0;result+=c;
    }
    result.append(2*slashes,L'\\');result+=L'"';return result;
}
struct Pipe {
    NativeHandle read,write;
    Pipe(){SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};native_check(CreatePipe(&read.value,&write.value,&sa,0)!=0,"CreatePipe");}
};
#else
struct NativeHandle {
    int value=-1;
    NativeHandle()=default;
    NativeHandle(const NativeHandle&)=delete;
    ~NativeHandle(){reset();}
    void reset(int next=-1){if(value>=0)::close(value);value=next;}
    int release(){int result=value;value=-1;return result;}
};
void native_check(int error,const char* what){if(error)throw std::system_error(error,std::generic_category(),what);}
struct Pipe {
    NativeHandle read,write;
    Pipe() {
        int fds[2];if(pipe2(fds,O_CLOEXEC)!=0)native_check(errno,"pipe2");
        read.value=fds[0];write.value=fds[1];
        // Keep file actions safe even when the caller has closed fd 0/1/2.
        for(auto* fd:{&read,&write})if(fd->value<3) {
            int copy=fcntl(fd->value,F_DUPFD_CLOEXEC,3);
            if(copy<0)native_check(errno,"duplicate pipe descriptor");
            fd->reset(copy);
        }
    }
};
struct SpawnActions {
    posix_spawn_file_actions_t value;
    SpawnActions(){native_check(posix_spawn_file_actions_init(&value),"spawn actions");}
    ~SpawnActions(){posix_spawn_file_actions_destroy(&value);}
};
struct SpawnAttributes {
    posix_spawnattr_t value;
    SpawnAttributes(){native_check(posix_spawnattr_init(&value),"spawn attributes");}
    ~SpawnAttributes(){posix_spawnattr_destroy(&value);}
};
#endif
}
struct ChildProcess::Impl {
    PipeLimits limits;
    std::shared_ptr<ProcessWakeup> wakeup;
    void notify_controller() noexcept {if(wakeup)wakeup->notify();}
    NativeHandle input,output,error;
#ifdef _WIN32
    NativeHandle process,job;
#else
    pid_t pid=-1;
#endif
    std::optional<int> code;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread writer,reader,logger;
    std::deque<std::string> incoming,outgoing;
    std::size_t incoming_bytes=0,outgoing_bytes=0;
    std::string diagnostic,fault;
    bool stopping=false,stdout_closed=false,truncated=false;
    ~Impl(){stop();}
    void fail(const char* message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!stopping && fault.empty())fault=message;
            changed.notify_all();
        }
        notify_controller();
    }
    void start(const std::string& executable,const std::vector<std::string>& args) {
        if(!std::filesystem::u8path(executable).is_absolute() || executable.find('\0')!=std::string::npos)
            throw std::invalid_argument("child executable must be an absolute path without NUL");
        for(const auto& arg:args)if(arg.find('\0')!=std::string::npos)throw std::invalid_argument("process argument contains NUL");
        Pipe in,out,err;
#ifdef _WIN32
        native_check(SetHandleInformation(in.write.value,HANDLE_FLAG_INHERIT,0)!=0,"parent stdin handle");
        native_check(SetHandleInformation(out.read.value,HANDLE_FLAG_INHERIT,0)!=0,"parent stdout handle");
        native_check(SetHandleInformation(err.read.value,HANDLE_FLAG_INHERIT,0)!=0,"parent stderr handle");
        job.reset(CreateJobObjectW(nullptr,nullptr));native_check(job.value!=nullptr,"CreateJobObject");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        native_check(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&info,sizeof(info))!=0,"job ownership");
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        if(!bytes)native_check(false,"attribute list size");
        std::vector<unsigned char> storage(bytes);
        auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        native_check(InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=0,"attribute list");
        struct Delete {LPPROC_THREAD_ATTRIBUTE_LIST list;~Delete(){DeleteProcThreadAttributeList(list);}} cleanup{attributes};
        HANDLE inherited[]{in.read.value,out.write.value,err.write.value};
        native_check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=0,"child handle list");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=in.read.value;startup.StartupInfo.hStdOutput=out.write.value;startup.StartupInfo.hStdError=err.write.value;
        auto application=wide(executable);auto command=quote(application);
        for(const auto& arg:args){command+=L' ';command+=quote(wide(arg));}
        if(command.size()>=32767)throw std::invalid_argument("Windows child command line exceeds limit");
        PROCESS_INFORMATION child{};
        native_check(CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&child)!=0,"CreateProcess");
        process.reset(child.hProcess);NativeHandle initial_thread;initial_thread.reset(child.hThread);
        native_check(AssignProcessToJobObject(job.value,process.value)!=0,"assign child to owned job");
        native_check(ResumeThread(initial_thread.value)!=DWORD(-1),"resume child");
#else
        struct sigaction child_action{};
        if(sigaction(SIGCHLD,nullptr,&child_action)!=0)native_check(errno,"SIGCHLD disposition");
        if(child_action.sa_handler==SIG_IGN || (child_action.sa_flags&SA_NOCLDWAIT))
            throw std::runtime_error("SIGCHLD auto-reaping prevents owned child lifetime tracking");
        SpawnActions actions;SpawnAttributes attr;
        for(auto pair:{std::make_pair(in.read.value,0),std::make_pair(out.write.value,1),std::make_pair(err.write.value,2)})
            native_check(posix_spawn_file_actions_adddup2(&actions.value,pair.first,pair.second),"redirect child pipe");
        for(int fd:{in.read.value,in.write.value,out.read.value,out.write.value,err.read.value,err.write.value})
            native_check(posix_spawn_file_actions_addclose(&actions.value,fd),"close child pipe copy");
        sigset_t empty,defaults;sigemptyset(&empty);sigemptyset(&defaults);
        for(int signal:{SIGPIPE,SIGINT,SIGTERM})sigaddset(&defaults,signal);
        native_check(posix_spawnattr_setsigmask(&attr.value,&empty),"child signal mask");
        native_check(posix_spawnattr_setsigdefault(&attr.value,&defaults),"child signal defaults");
        native_check(posix_spawnattr_setpgroup(&attr.value,0),"child process group");
        native_check(posix_spawnattr_setflags(&attr.value,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF),"child spawn flags");
        std::vector<std::string> values{executable};values.insert(values.end(),args.begin(),args.end());
        std::vector<char*> argv;for(auto& value:values)argv.push_back(value.data());argv.push_back(nullptr);
        pid_t spawned=-1;
        native_check(posix_spawn(&spawned,executable.c_str(),&actions.value,&attr.value,argv.data(),environ),"posix_spawn");
        pid=spawned;
#endif
        input.reset(in.write.release());output.reset(out.read.release());error.reset(err.read.release());
        // Drop the parent's copies of the child's endpoints before starting I/O.
        in.read.reset();out.write.reset();err.write.reset();
#ifndef _WIN32
        for(auto* handle:{&input,&output,&error}) {
            int flags=fcntl(handle->value,F_GETFL);
            if(flags<0 || fcntl(handle->value,F_SETFL,flags|O_NONBLOCK)<0)native_check(errno,"nonblocking parent pipe");
        }
#endif
        reader=std::thread([this]{try {read_output();}catch(...){fail("stdout pump failed");}});
        logger=std::thread([this]{try {read_error();}catch(...){fail("stderr pump failed");}});
        writer=std::thread([this]{try {write_input();}catch(...){fail("stdin pump failed");}});
    }
    std::ptrdiff_t read_pipe(NativeHandle& handle,char* data,std::size_t size) {
#ifdef _WIN32
        DWORD count=0;if(ReadFile(handle.value,data,DWORD(size),&count,nullptr))return count;
        return GetLastError()==ERROR_BROKEN_PIPE?0:-1;
#else
        for(;;) {
            auto count=::read(handle.value,data,size);
            if(count>=0)return count;
            if(errno==EINTR)continue;
            if(errno!=EAGAIN && errno!=EWOULDBLOCK)return -1;
            {std::lock_guard<std::mutex> lock(mutex);if(stopping)return 0;}
            pollfd descriptor{handle.value,POLLIN,0};
            if(poll(&descriptor,1,50)<0 && errno!=EINTR)return -1;
        }
#endif
    }
    void read_output() {
        std::array<char,8192> buffer;std::string pending;
        for(;;) {
            auto count=read_pipe(output,buffer.data(),buffer.size());
            if(count<=0) {
                if(count<0)fail("stdout read failed");
                else if(!pending.empty())fail("truncated stdout frame");
                break;
            }
            for(std::ptrdiff_t i=0;i<count;++i) {
                if(buffer[std::size_t(i)]!='\n') {
                    if(pending.size()>=limits.line_bytes+1){fail("stdout frame exceeds limit");return;}
                    pending+=buffer[std::size_t(i)];continue;
                }
                if(!pending.empty() && pending.back()=='\r')pending.pop_back();
                if(pending.size()>limits.line_bytes){fail("stdout frame exceeds limit");return;}
                bool overflow=false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(stopping)return;
                    if(incoming.size()>=limits.queued_lines || pending.size()+1>limits.queued_bytes-incoming_bytes) {
                        if(fault.empty())fault="stdout queue exceeds limit";
                        overflow=true;
                    } else {
                        incoming_bytes+=pending.size()+1;incoming.push_back(std::move(pending));
                        pending.clear();
                    }
                    changed.notify_all();
                }
                notify_controller();
                if(overflow)return;
            }
        }
        {std::lock_guard<std::mutex> lock(mutex);stdout_closed=true;changed.notify_all();}
        notify_controller();
    }
    void read_error() {
        std::array<char,8192> buffer;
        for(;;) {
            auto count=read_pipe(error,buffer.data(),buffer.size());
            if(count<=0){if(count<0)fail("stderr read failed");return;}
            std::lock_guard<std::mutex> lock(mutex);
            auto size=std::size_t(count);
            if(size>limits.stderr_bytes-diagnostic.size()) {
                truncated=true;
                auto discard=size-(limits.stderr_bytes-diagnostic.size());
                diagnostic.erase(0,std::min(discard,diagnostic.size()));
                if(size>limits.stderr_bytes){diagnostic.assign(buffer.data()+size-limits.stderr_bytes,limits.stderr_bytes);continue;}
            }
            diagnostic.append(buffer.data(),size);
        }
    }
    void write_input() {
#ifndef _WIN32
        // EPIPE must be an I/O failure in this thread, not terminate the parent.
        sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);
        native_check(pthread_sigmask(SIG_BLOCK,&blocked,nullptr),"writer SIGPIPE mask");
#endif
        for(;;) {
            std::string frame;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock,[&]{return stopping || !outgoing.empty();});
                if(stopping)return;
                frame=std::move(outgoing.front());outgoing.pop_front();outgoing_bytes-=frame.size();
            }
            std::size_t offset=0;
            while(offset<frame.size()) {
#ifdef _WIN32
                DWORD count=0;
                if(!WriteFile(input.value,frame.data()+offset,DWORD(std::min<std::size_t>(frame.size()-offset,65536)),&count,nullptr) || !count) {
                    fail("stdin write failed");return;
                }
#else
                ssize_t count=::write(input.value,frame.data()+offset,frame.size()-offset);
                if(count<0 && errno==EINTR)continue;
                if(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
                    {std::lock_guard<std::mutex> lock(mutex);if(stopping)return;}
                    pollfd descriptor{input.value,POLLOUT,0};
                    if(poll(&descriptor,1,50)<0 && errno!=EINTR){fail("stdin poll failed");return;}
                    continue;
                }
                if(count<=0){fail("stdin write failed");return;}
#endif
                offset+=std::size_t(count);
            }
        }
    }
    std::optional<int> exit_code() {
        if(code)return code;
#ifdef _WIN32
        if(!process.value || WaitForSingleObject(process.value,0)!=WAIT_OBJECT_0)return {};
        DWORD status=0;native_check(GetExitCodeProcess(process.value,&status)!=0,"child exit status");code=int(status);
#else
        if(pid<0)return {};
        siginfo_t info{};int result;
        do {result=waitid(P_PID,id_t(pid),&info,WEXITED|WNOHANG|WNOWAIT);}while(result<0 && errno==EINTR);
        if(result<0)native_check(errno,"child exit status");
        if(!info.si_pid)return {};
        code=info.si_code==CLD_EXITED?info.si_status:128+info.si_status;
        // WNOWAIT keeps the PID reserved until stop() kills the owned group.
#endif
        return code;
    }
    void stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;outgoing.clear();outgoing_bytes=0;changed.notify_all();}
        notify_controller();
#ifdef _WIN32
        if(job.value)TerminateJobObject(job.value,1);
        if(process.value && WaitForSingleObject(process.value,0)!=WAIT_OBJECT_0)TerminateProcess(process.value,1);
        if(process.value)WaitForSingleObject(process.value,INFINITE);
#else
        if(pid>0) {
            // Direct child has not been reaped: the group id cannot be reused.
            // Refuse to signal a group if an external reaper broke ownership.
            siginfo_t info{};int owned;
            do {owned=waitid(P_PID,id_t(pid),&info,WEXITED|WNOHANG|WNOWAIT);}while(owned<0 && errno==EINTR);
            if(owned==0) {
                ::kill(-pid,SIGKILL);int status=0;pid_t result;
                do {result=waitpid(pid,&status,0);}while(result<0 && errno==EINTR);
                if(result==pid && !code)code=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
            }
            pid=-1;
        }
#endif
        for(auto* thread:{&writer,&reader,&logger})if(thread->joinable())thread->join();
        input.reset();output.reset();error.reset();
    }
};
ChildProcess::ChildProcess(const std::string& executable,const std::vector<std::string>& args,PipeLimits limits,std::shared_ptr<ProcessWakeup> wakeup):impl_(std::make_unique<Impl>()) {
    if(!limits.line_bytes || limits.line_bytes>16*1024*1024 || !limits.queued_bytes || limits.queued_bytes>64*1024*1024 ||
       !limits.queued_lines || limits.queued_lines>1024 || limits.stderr_bytes>1024*1024)
        throw std::invalid_argument("invalid bounded pipe limits");
    impl_->limits=limits;impl_->wakeup=std::move(wakeup);impl_->start(executable,args);
}
ChildProcess::~ChildProcess()=default;
void ChildProcess::send(std::string line) {
    if(line.size()>impl_->limits.line_bytes || line.find_first_of("\r\n")!=std::string::npos)
        throw std::invalid_argument("invalid outbound frame");
    line+='\n';std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->stopping || impl_->stdout_closed || !impl_->fault.empty())throw std::runtime_error("child pipe is closed or faulted");
    if(impl_->outgoing.size()>=impl_->limits.queued_lines || line.size()>impl_->limits.queued_bytes-impl_->outgoing_bytes)
        throw std::runtime_error("stdin queue exceeds limit");
    impl_->outgoing_bytes+=line.size();impl_->outgoing.push_back(std::move(line));impl_->changed.notify_all();
}
PipeLine ChildProcess::read(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if(!impl_->changed.wait_until(lock,deadline,[&]{return !impl_->fault.empty() || !impl_->incoming.empty() || impl_->stdout_closed || impl_->stopping;}))
        return {PipeLine::State::timeout,{}};
    if(!impl_->fault.empty())return {PipeLine::State::fault,impl_->fault};
    if(impl_->incoming.empty())return {PipeLine::State::eof,{}};
    auto text=std::move(impl_->incoming.front());impl_->incoming.pop_front();impl_->incoming_bytes-=text.size()+1;
    return {PipeLine::State::line,std::move(text)};
}
std::optional<int> ChildProcess::exit_code(){return impl_->exit_code();}
std::string ChildProcess::stderr_text() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->diagnostic;}
bool ChildProcess::stderr_truncated() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->truncated;}
std::string ChildProcess::pipe_fault() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->fault;}
void ChildProcess::stop() noexcept {impl_->stop();}
}
