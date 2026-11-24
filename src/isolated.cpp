#include "isolated.h"
#include "budget.h"
#include "c2_resize.h"
#include "child_process.h"
#include "files.h"
#include "worker_protocol.h"
#include "error_registry.h"
#include "selection_request.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace thumbgen::detail {
#if defined(THUMBGEN_TEST_HOOKS)
namespace {
// Test-only: the notification tests install a hook that the controller thread
// runs at the no-progress scan/wait boundary. Never set outside test builds.
std::function<void(const std::shared_ptr<ProcessWakeup>&)> test_scan_hook;
}
void set_test_scan_hook(std::function<void(const std::shared_ptr<ProcessWakeup>&)> hook){test_scan_hook=std::move(hook);}
#endif
namespace {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void check(bool ok,const char* reason){if(!ok)throw ProtocolError(reason);}
void fields(const Json& object,std::initializer_list<const char*> names) {
    check(object.is_object() && object.size()==names.size(),"invalid worker payload fields");
    for(auto name:names)check(object.contains(name),"missing worker payload field");
}
void allowed(const Json& object,std::initializer_list<const char*> names) {
    check(object.is_object(),"worker field must be an object");
    for(auto it=object.begin();it!=object.end();++it)check(std::any_of(names.begin(),names.end(),[&](const char* name){return it.key()==name;}),"unknown worker payload field");
}
void error_check(const Json& value) {
    allowed(value,{"code","message","retryable","stage"});
    check(value.contains("code") && value["code"].is_string() && value.contains("message") && value["message"].is_string(),"invalid worker error shape");
    static const auto codes=[] {std::set<std::string> result;auto registry=Json::parse(error_registry_json);for(const auto& item:registry["errors"])result.insert(item["code"].get<std::string>());return result;}();
    check(codes.count(value["code"].get<std::string>())!=0,"unregistered worker error code");
    if(value.contains("retryable"))check(value["retryable"].is_boolean(),"invalid worker retryability");
    if(value.contains("stage"))check(value["stage"]=="open" || value["stage"]=="selection" || value["stage"]=="colour" || value["stage"]=="decode" || value["stage"]=="visualization","invalid worker error stage");
}
std::uint64_t number(const Json& value,std::uint64_t maximum=std::numeric_limits<std::uint64_t>::max()) {
    check(value.is_number_integer() && value.get<long double>()>=0 && value.get<long double>()<=maximum,"invalid worker payload integer");
    return value.get<std::uint64_t>();
}
Json identity() {
    auto capabilities=Engine::capabilities();
    return {{"build",capabilities["build"]},{"version",capabilities["version"]},{"oiio_version",capabilities["oiio_version"]}};
}
std::string clean(const std::string& value){return Json::parse(Json(value).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();}
Json error(const FileFailure& e){return {{"code",e.code},{"message",clean(e.what())},{"retryable",e.retryable}};}
std::string stop_diagnostic(ChildProcess& process,const std::string& reason) {
    std::optional<int> exit;try {exit=process.exit_code();}catch(const std::exception&){}
    process.stop();std::string message=reason;if(exit)message+="; exit_status="+std::to_string(*exit);
    auto diagnostic=process.stderr_text();if(!diagnostic.empty())message+="; worker stderr: "+diagnostic;
    if(process.stderr_truncated())message+=" [stderr truncated]";
    return clean(message);
}
void remote_error(const Json& payload) {
    if(!payload.contains("error"))return;
    const auto& value=payload["error"];fields(value,{"code","message","retryable"});error_check(value);
    check(value["code"].is_string() && value["message"].is_string() && value["retryable"].is_boolean(),"invalid worker error");
    throw FileFailure(value["code"].get<std::string>(),value["message"].get<std::string>(),value["retryable"].get<bool>());
}
std::optional<std::string> input_frame() {
    std::string line;char c;
    while(std::cin.get(c)) {
        if(c=='\n'){if(!line.empty() && line.back()=='\r')line.pop_back();return line;}
        if(line.size()>=WorkerSession::max_frame)throw ProtocolError("worker stdin frame exceeds limit");
        line+=c;
    }
    if(!line.empty())throw ProtocolError("truncated worker stdin frame");
    return {};
}
template<class Fn> decltype(auto) guarded(WorkerSession& session,Fn fn) {
    try {return fn();}catch(const ProtocolError& e){session.reject(e.what());}
}
Json stamp_json(const SourceStamp& stamp) {
    return {{"identity",stamp.identity},{"size",stamp.size},{"modified_seconds",stamp.modified_seconds},{"modified_fraction",stamp.modified_fraction}};
}
Json failed_output(const Output& output,const Json& failure) {
    return {{"path",output.path},{"existing_policy",output.existing},{"status","failed"},{"error",failure}};
}
Json task_base(const Job& job) {
    Json value{{"schema_version",1},{"type","result"},{"id",job.id},{"input",job.input},
        {"status","failed"},{"ok",false},{"outputs",Json::array()},{"warnings",Json::array()},{"timings_ms",Json::object()}};
    if(!job.sequence_id.empty())value["sequence_id"]=job.sequence_id;
    return value;
}
std::string effective_visualization(const Json& value,const Job& job) {
    if(job.options.visualization=="auto")return value.contains("cryptomatte")?"cryptomatte":"colour";
    return job.options.visualization;
}
void base_check(const Json& value,const Job& job) {
    allowed(value,{"schema_version","type","id","input","ok","status","outputs","warnings","error","selection","source","selected_channels","colour","decoded_mib","preset","filter","timings_ms","sequence_id","source_check","visualization","cryptomatte","source_read","deep"});
    check(value.is_object() && value.value("type",std::string{})=="result" && value.value("id",std::string{})==job.id &&
        value.value("input",std::string{})==job.input && value.value("schema_version",0)==1,"worker result identity mismatch");
    check(value.value("sequence_id",std::string{})==job.sequence_id,"worker sequence identity mismatch");
    check(value.contains("warnings") && value["warnings"].is_array() && value.contains("timings_ms") && value["timings_ms"].is_object(),"invalid worker result metadata");
    for(const auto& warning:value["warnings"])check(warning.is_string(),"invalid worker warning");
    if(value.contains("error"))error_check(value["error"]);
    const auto mode=effective_visualization(value,job);
    const bool automatic=job.options.visualization=="auto";
    if(automatic && value.contains("source")) {
        const auto& chosen=value.at("selection").at("chosen");
        check(chosen["kind"]=="colour" || chosen["kind"]=="grey" || chosen["kind"]=="data","invalid worker automatic selection");
        check((chosen["kind"]=="data")==value.contains("cryptomatte"),"worker automatic selection does not match its preview mode");
    }
    if(value.contains("source") && value["source"].value("deep",false) && job.options.deep_mode=="flatten")
        check(value.contains("deep"),"missing worker Deep diagnostics");
    if(value.contains("deep")) {
        const auto& deep=value["deep"];fields(deep,{"mode","policy","mapping","sample_limit","buffer_limit_bytes","samples","chunks","peak_buffer_bytes"});
        check(job.options.deep_mode=="flatten" && mode=="colour" && deep["mode"]=="flatten" && deep["policy"]=="deep-colour-sweep-v1" &&
            deep["sample_limit"]==job.options.deep_samples_per_pixel && deep["buffer_limit_bytes"]==job.options.max_decode_mib*1024ULL*1024ULL &&
            value.contains("source") && value["source"].value("deep",false),"worker Deep settings mismatch");
        number(deep["samples"],64ULL*1024*1024);number(deep["chunks"],std::numeric_limits<std::uint64_t>::max());
        number(deep["peak_buffer_bytes"],job.options.max_decode_mib*1024ULL*1024ULL);
        // Reuse the strict sequence-key validator, including exact alpha/Z mappings.
        check(value.contains("selection") && value["selection"].contains("chosen"),"missing Deep selection");
        if(!job.sequence_id.empty() && value["selection"].contains("locked"))
            check(value["selection"]["locked"].contains("deep") && value["selection"]["locked"]["deep"]==deep["mapping"],"worker Deep sequence mapping mismatch");
        auto key=value["selection"]["chosen"];key.erase("reason");key.erase("primary");
        if(key["part_name"]!="")key.erase("part");
        else key["part"]=0; // A non-sequence multipart index is not a sequence identity.
        key["deep"]=deep["mapping"];
        auto request=selection_request(job);if(key["part_name"]=="")request["part"]=-1;
        Engine verifier;try{verifier.restore_sequence_lock("validate",{{"key",key},{"request",request},{"named",key["part_name"]!=""}});}
        catch(const std::exception&){check(false,"invalid worker Deep channel mapping");}
    }
    if(value.contains("source_read")) {
        const auto& read=value["source_read"];fields(read,{"mode","grid","block","calls","buffer_bytes"});
        check(job.options.visualization=="id" && read["mode"]=="sampled_blocks","worker source read mode mismatch");
        for(const char* key:{"grid","block"}) {
            check(read[key].is_array() && read[key].size()==2,"invalid sampled source dimensions");
            for(const auto& n:read[key]){auto count=number(n,2147483647);check(key[0]=='g' || count>0,"invalid sampled source block");}
        }
        number(read["calls"],std::numeric_limits<std::uint64_t>::max());
        check(number(read["buffer_bytes"],std::numeric_limits<std::uint64_t>::max())>0,"missing sampled source buffer");
    }
    if(value.contains("cryptomatte")) {
        const auto& crypto=value["cryptomatte"];fields(crypto,{"name","metadata_key","hash","conversion","layers","ranks","manifest"});
        if(automatic) {
            const auto& chosen=value.at("selection").at("chosen");
            check(auto_cryptomatte_allowed(job.options) && value.contains("source") && !value["source"].value("deep",true) &&
                chosen["kind"]=="data" && chosen["group"]==crypto["name"] && chosen["channels"]==value.at("selected_channels"),
                "worker automatic Cryptomatte selection mismatch");
        }
        check(mode=="cryptomatte" && (automatic || crypto["name"]==job.options.channels) && crypto["name"].is_string() &&
            !crypto["name"].get_ref<const std::string&>().empty() && crypto["hash"]=="MurmurHash3_32" &&
            crypto["conversion"]=="uint32_to_float32" && crypto["metadata_key"].is_string() && !crypto["metadata_key"].get<std::string>().empty(),"worker Cryptomatte metadata mismatch");
        check(crypto["layers"].is_array() && !crypto["layers"].empty(),"missing Cryptomatte layers");
        for(const auto& layer:crypto["layers"])check(layer.is_string() && !layer.get<std::string>().empty(),"invalid Cryptomatte layer");
        auto ranks=number(crypto["ranks"],2147483647);
        check((ranks==2*crypto["layers"].size() || ranks+1==2*crypto["layers"].size()) && value.contains("selected_channels") &&
            value["selected_channels"].is_array() && value["selected_channels"].size()==2*ranks,"invalid Cryptomatte rank count");
        const auto& manifest=crypto["manifest"];fields(manifest,{"embedded_attribute","sidecar_attribute","used"});
        check(manifest["embedded_attribute"].is_boolean() && manifest["sidecar_attribute"].is_boolean() && manifest["used"]==false,"invalid Cryptomatte manifest diagnostics");
    }
    if(mode=="cryptomatte" && (value.value("status",std::string{})=="success" || value.value("status",std::string{})=="partial" || value.value("status",std::string{})=="skipped"))
        check(value.contains("cryptomatte"),"missing worker Cryptomatte metadata");
    if(value.contains("visualization")) {
        const auto& data=value["visualization"];fields(data,{"mode","policy","range","invalid_values","invalid_pixels","clipped_components"});
        check(mode!="colour" && data["mode"]==mode && data["policy"]=="v1" && data["invalid_values"]==job.options.invalid_values &&
            data["range"]==(automatic?Json::array():selection_request(job)["data"]["range"]),"worker visualization settings mismatch");
        number(data["invalid_pixels"],std::numeric_limits<std::uint64_t>::max());number(data["clipped_components"],std::numeric_limits<std::uint64_t>::max());
    }
    if(mode!="colour" && (value.value("status",std::string{})=="success" || value.value("status",std::string{})=="partial"))
        check(value.contains("visualization"),"missing worker visualization result");
}
void output_check(const Json& value,const Output& output) {
    check(value.is_object() && value.value("path",std::string{})==output.path && value.value("existing_policy",std::string{})==output.existing,"worker output identity mismatch");
    auto status=value.value("status",std::string{});
    if(status=="success") {
        fields(value,{"path","status","width","height","format","existing_policy","resize_ms","finish_ms"});
        check(number(value["width"],output.width)>0 && number(value["height"],output.height)>0 && value["format"]==output.format,"invalid worker output dimensions/format");
        check(value["finish_ms"].is_number() && std::isfinite(value["finish_ms"].get<double>()) && value["finish_ms"].get<double>()>=0,"invalid worker finish time");
    } else if(status=="skipped") {
        allowed(value,{"path","status","existing_policy","reason","resize_ms"});
        check(output.existing=="skip" && value.contains("reason") && value["reason"]=="output_exists","invalid worker skip");
    } else if(status=="failed") {
        allowed(value,{"path","status","existing_policy","error","resize_ms"});check(value.contains("error"),"missing worker output error");error_check(value["error"]);
    } else throw ProtocolError("invalid worker output status");
    if(value.contains("resize_ms"))check(value["resize_ms"].is_number() && std::isfinite(value["resize_ms"].get<double>()) && value["resize_ms"].get<double>()>=0,"invalid worker resize time");
}
class Channel {
public:
    WorkerSession session;
    ChildProcess process;
    unsigned threads;
    std::string id;
    bool ready=false;
    Channel(const std::string& exe,const std::string& token,const std::string& worker,unsigned quota,std::chrono::milliseconds,std::shared_ptr<ProcessWakeup> wakeup):
        session(token,worker),process(exe,{"--internal-worker",token,worker,std::to_string(quota)},{},std::move(wakeup)),threads(quota),id(worker) {
        try {
        send("hello","",{{"identity",identity()},{"threads",quota},{"parent_pid",
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        }});
        }catch(const ProtocolError& e){throw ProtocolError(stop_diagnostic(process,e.what()));}
         catch(const std::exception& e){throw FileFailure("worker_start_failed",stop_diagnostic(process,e.what()));}
    }
    void send(const std::string& type,const std::string& task,const Json& payload){process.send(session.encode(type,task,payload));}
    std::optional<WorkerMessage> poll(Clock::time_point deadline) {
        if(Clock::now()>=deadline)throw FileFailure(ready?"worker_timeout":"worker_start_failed","worker deadline exceeded",true);
        auto frame=process.read(Clock::now());
        if(frame.state==PipeLine::State::timeout)return {};
        if(frame.state==PipeLine::State::eof)throw FileFailure(ready?"worker_crashed":"worker_start_failed","worker exited before completing the task");
        if(frame.state==PipeLine::State::fault) {
            if(!ready)throw FileFailure("worker_start_failed",frame.text);
            session.reject(frame.text);
        }
        auto message=session.decode(frame.text);
        if(!ready) {
            check(message.type=="hello_ack" && message.task_id.empty(),"invalid worker hello reply");
            fields(message.payload,{"identity","threads"});check(message.payload["identity"]==identity() && number(message.payload["threads"],256)==threads,"worker build/thread mismatch");
            ready=true;
        }
        return message;
    }
};
}
int serve_worker(const std::string& token,const std::string& worker,unsigned threads,Engine::Observer observer) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#else
    struct rlimit core{};if(getrlimit(RLIMIT_CORE,&core)==0){core.rlim_cur=0;setrlimit(RLIMIT_CORE,&core);}
#endif
    try {
        check(threads>=1 && threads<=256,"invalid worker thread quota");WorkerSession session(token,worker);
        auto receive=[&](const std::string& type,const std::string& task) {
            auto frame=input_frame();if(!frame)session.reject("worker control pipe closed");auto message=session.decode(*frame);
            if(message.type!=type || message.task_id!=task)session.reject("unexpected worker control state");
            return message.payload;
        };
        auto send=[&](const std::string& type,const std::string& task,const Json& payload){std::cout<<session.encode(type,task,payload)<<'\n'<<std::flush;if(!std::cout)session.reject("worker stdout failed");};
        auto hello=receive("hello","");fields(hello,{"identity","threads","parent_pid"});check(hello["identity"]==identity() && number(hello["threads"],256)==threads,"worker hello mismatch");
        check(number(hello["parent_pid"],4294967295ULL)>0,"invalid worker parent pid");
#ifndef _WIN32
        auto parent=pid_t(number(hello["parent_pid"],2147483647));
        check(getppid()==parent,"worker parent already exited");
        if(prctl(PR_SET_PDEATHSIG,long(SIGKILL),0L,0L,0L)!=0)throw std::system_error(errno,std::generic_category(),"worker parent-death signal");
        check(getppid()==parent,"worker parent exited during lifetime registration");
#endif
        send("hello_ack","",{{"identity",identity()},{"threads",threads}});Engine engine(observer);
        for(;;) {
            auto frame=input_frame();if(!frame)return 0;auto message=session.decode(*frame);
            if(message.type=="shutdown" && message.task_id.empty()){fields(message.payload,{});send("shutdown_ack","",Json::object());return 0;}
            check(message.type=="prepare","worker expected prepare");fields(message.payload,{"job","pin"});
            auto job=job_from_wire(message.payload["job"]);check(message.task_id==job.id && unsigned(job.options.threads)==threads,"worker job/quota mismatch");
            if(!message.payload["pin"].is_null())engine.restore_sequence_lock(job.sequence_id,message.payload["pin"]);
            std::map<size_t,std::pair<std::string,std::string>> borrowed;
            Engine::Control control;
            control.planned=[&](const Job&,const Json& plan){guarded(session,[&]{if(observer)observer("planned",job);send("planned",job.id,plan);auto reply=receive("run",job.id);remote_error(reply);fields(reply,{});});};
            control.reserve=[&](const Job&,size_t index) {return guarded(session,[&] {
                send("reserve",job.id,{{"output_index",index}});auto reply=receive("reserved",job.id);
                check(reply.contains("output_index") && number(reply["output_index"])==index,"reserved output mismatch");remote_error(reply);
                fields(reply,{"output_index","temporary_id","path"});check(reply["temporary_id"].is_string() && reply["path"].is_string(),"invalid temporary reservation");
                auto value=std::make_pair(reply["temporary_id"].get<std::string>(),reply["path"].get<std::string>());check(borrowed.emplace(index,value).second,"duplicate reserved output");return value.second;
            });};
            control.commit=[&](const Job&,size_t index,const std::string& path,const Json& encoded) {return guarded(session,[&] {
                check(borrowed.count(index) && borrowed.at(index).second==path,"unknown temporary output");
                send("encoded",job.id,{{"output_index",index},{"temporary_id",borrowed.at(index).first},{"encoded",encoded}});
                auto reply=receive("committed",job.id);check(reply.contains("output_index") && number(reply["output_index"])==index,"commit output mismatch");remote_error(reply);
                fields(reply,{"output_index","status"});check(reply["status"]=="success" || reply["status"]=="skipped","invalid commit status");
                if(observer)observer("committed",job);
                return reply["status"]=="success";
            });};
            engine.set_control(std::move(control));auto record=engine.convert(job);
            if(session.failed())return 1;
            send("result",job.id,{{"record",record},{"pin",engine.sequence_lock(job.sequence_id)}});
            engine.forget_sequence_lock(job.sequence_id);
        }
    }catch(const std::exception& e){std::cerr<<"thumbgen worker: "<<e.what()<<'\n';return 1;}
}
Json execution_configuration(const std::vector<Job>& jobs,const IsolationOptions& options) {
    if(options.memory_mib>2147483647ULL || options.thread_budget>256 || !options.workers || options.workers>256 || options.startup_timeout.count()<1 || options.startup_timeout.count()>86400000 ||
       options.task_timeout.count()<0 || options.task_timeout.count()>86400000)throw std::invalid_argument("invalid execution configuration");
    unsigned thread_budget=options.thread_budget;
    if(!thread_budget){thread_budget=1;for(const auto& job:jobs)thread_budget=std::max(thread_budget,unsigned(job.options.threads));}
    if(thread_budget>256)throw std::invalid_argument("invalid requested thread quota");
    if(options.workers>1 && (!options.memory_mib || !options.thread_budget))throw std::invalid_argument("multiple workers require explicit positive memory and thread budgets");
    const auto workers=std::min({options.workers,thread_budget,unsigned(std::min<size_t>(256,std::max<size_t>(1,jobs.size())))});
    // Retained/preparing workers are charged even while no task holds admission.
    // Admission charges only incremental task buffers, never this allowance twice.
    const std::uint64_t fixed_reserve=128+std::uint64_t(workers)*(128+8*(thread_budget/workers));
    return {{"workers",workers},{"requested_workers",options.workers},{"thread_budget",thread_budget},{"memory_budget_mib",options.memory_mib},{"fixed_reserve_mib",fixed_reserve},
        {"startup_timeout_ms",options.startup_timeout.count()},{"task_timeout_ms",options.task_timeout.count()}};
}
bool run_isolated(const std::vector<Job>& jobs,const std::string& executable,const std::function<void(const Json&)>& emit,IsolationOptions options) {
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
    const auto profile_batch_started=Clock::now();
#endif
    const auto wakeup=std::make_shared<ProcessWakeup>();
    for(const auto& job:jobs)job_from_wire(job_to_wire(job));
    const auto configuration=execution_configuration(jobs,options);
    const auto thread_budget=configuration["thread_budget"].get<unsigned>();
    const auto workers=configuration["workers"].get<unsigned>();
    const auto fixed_reserve=configuration["fixed_reserve_mib"].get<std::uint64_t>();
    const std::uint64_t memory=options.memory_mib?std::uint64_t(options.memory_mib)*1048576:std::numeric_limits<std::uint64_t>::max();
    std::unique_ptr<BudgetQueue> budget;
    if(memory>fixed_reserve*1048576)budget=std::make_unique<BudgetQueue>(BudgetLimits{workers,thread_budget,memory,fixed_reserve*1048576});
    struct Task {
        Job job;size_t index;Clock::time_point started=Clock::now(),deadline,queued_at;
        Json base,result;std::optional<double> estimate_mib,admission_mib;std::string worker_id;
        std::vector<std::optional<Json>> ledger;
        struct Owned {std::string id;std::unique_ptr<TemporaryOutput> file;};
        std::map<size_t,Owned> owned;std::set<size_t> reserved;std::optional<SourceStamp> source;
        bool initialized=false,planned=false,ran=false,admitted=false,sent=false,queued=false,finished=false,c2_stream=false;
        double queue_ms=0;
        Task(Job value,size_t ordinal):job(std::move(value)),index(ordinal),base(task_base(job)),ledger(job.outputs.size()){}
    };
    struct Lane {
        std::unique_ptr<Channel> channel;std::unique_ptr<Task> task;
        // An exception in emit/admission must stop writers before borrowed files die.
        ~Lane(){channel.reset();task.reset();}
    };
    std::vector<Lane> lanes(workers);
    std::random_device random;std::string token;for(int i=0;i<32;++i)token+="0123456789abcdef"[random()&15];
    std::uint64_t worker_serial=0,temp_serial=0;Engine pins;std::map<std::string,Json> requests;
    std::map<std::string,size_t> remaining;for(const auto& job:jobs)if(!job.sequence_id.empty())++remaining[job.sequence_id];
    size_t next=0,completed=0;std::optional<size_t> preparing;
    bool interrupted=false;
    auto cancelled=[&] {if(!interrupted && options.cancelled && options.cancelled())interrupted=true;return interrupted;};
    while(completed<jobs.size()) {
        const auto observed=wakeup->sequence();
        bool progress=false;
        if(cancelled())for(auto& lane:lanes)if(lane.channel)lane.channel->process.stop();
        // Only one metadata preparation at a time: pins and FIFO order follow the manifest.
        if(!preparing && next<jobs.size())for(auto& lane:lanes)if(!lane.task) {
            auto job=jobs[next];job.options.threads=int(std::min(unsigned(job.options.threads),std::max(1u,thread_budget/workers)));
            lane.task=std::make_unique<Task>(std::move(job),next);preparing=next++;progress=true;break;
        }
        for(auto& lane:lanes) {
            if(!lane.task)continue;
            auto& task=*lane.task;auto& job=task.job;const auto& requested_job=jobs[task.index];
            auto& channel=lane.channel;auto& base=task.base;auto& result=task.result;
            auto& ledger=task.ledger;auto& owned=task.owned;auto& reserved=task.reserved;auto& source=task.source;
            auto& planned=task.planned;auto& ran=task.ran;auto& admitted=task.admitted;auto& sent=task.sent;auto& queued=task.queued;
            auto& deadline=task.deadline;auto& estimate_mib=task.estimate_mib;auto& worker_id=task.worker_id;auto started=task.started;
            auto accept_pin=[&](const Json& pin) {
                if(job.sequence_id.empty()){check(pin.is_null(),"unexpected sequence pin");return;}
                if(!pin.is_null()) {
                    check(pin.at("request")==selection_request(job),"worker sequence request mismatch");
                    pins.restore_sequence_lock(job.sequence_id,pin);
                }
                else check(pins.sequence_lock(job.sequence_id).is_null(),"worker lost sequence pin");
            };
            auto cleanup=[&](size_t index) {
                auto item=owned.find(index);if(item==owned.end())return;
                auto warning=item->second.file->cleanup();if(!warning.empty())base["warnings"].push_back("temporary cleanup failed: "+warning);
                owned.erase(item);
            };
            try {
                if(cancelled())throw FileFailure("task_cancelled","controller accepted interruption");
                if(!task.initialized) {
                    if(!job.sequence_id.empty()) {
                        Json request=selection_request(job);
                        auto previous=requests.find(job.sequence_id);
                        if(previous!=requests.end() && previous->second!=request)throw FileFailure("sequence_selection_conflict","selection options differ within sequence_id");
                        requests.emplace(job.sequence_id,std::move(request));
                    }
                    if(!budget)throw FileFailure("task_memory_exceeds_budget","budget does not cover fixed controller/IPC/idle-worker reserve");
                    if(!channel || channel->threads!=unsigned(job.options.threads)) {
                        channel.reset();
                        try {channel=std::make_unique<Channel>(executable,token,"w"+std::to_string(++worker_serial),unsigned(job.options.threads),options.startup_timeout,wakeup);}
                        catch(const ProtocolError&){throw;}
                        catch(const std::system_error& e){throw FileFailure("worker_start_failed",error_message(e.code()));}
                        catch(const std::exception& e){throw FileFailure("worker_start_failed",e.what());}
                    }
                    task.initialized=true;worker_id=channel->id;deadline=Clock::now()+options.startup_timeout;progress=true;
                }
                if(channel->ready && !sent) {
                    channel->send("prepare",job.id,{{"job",job_to_wire(job)},{"pin",pins.sequence_lock(job.sequence_id)}});
                    sent=true;deadline=Clock::now()+options.startup_timeout;progress=true;
                }
                auto received=channel->poll(deadline);
                if(!received)continue;
                progress=true;auto message=std::move(*received);
                if(!sent){check(message.type=="hello_ack","expected worker handshake");continue;}
                check(message.task_id==job.id,"worker task id mismatch");const auto& payload=message.payload;
                if(message.type=="planned") {
                    check(!planned,"duplicate worker plan");fields(payload,{"base","source_stamp","footprint","sequence_pin"});base_check(payload["base"],job);base=payload["base"];planned=true;accept_pin(payload["sequence_pin"]);
                    Json reply=Json::object();
                    try {
                        try {source=source_stamp(fs::u8path(job.input));}
                        catch(const FileFailure&){throw FileFailure("source_changed","source could not be verified after metadata preparation",true);}
                        if(stamp_json(*source)!=payload["source_stamp"])throw FileFailure("source_changed","source changed during metadata preparation",true);
                        const auto mode=effective_visualization(base,job);
                        const auto& f=payload["footprint"];fields(f,{"data_width","data_height","display_width","display_height","channel_span","selected_channels","output_width","output_height","threads","source_sample_bytes","source_pixel_bytes","source_buffer_bytes","native_categorical","deep_buffer_bytes","cms_buffer_bytes","srgb8","integer_linear","c2_stream","source_codec_bytes","bounded_read"});
                        const bool categorical=job.options.visualization=="id" || job.options.visualization=="id_rgb";
                        check(f["native_categorical"].is_boolean() && f["native_categorical"]==categorical,"worker categorical footprint mismatch");
                        check(number(f["threads"],256)==unsigned(job.options.threads),"plan thread quota mismatch");
                        ImageFootprint shape{number(f["data_width"],2147483647),number(f["data_height"],2147483647),number(f["display_width"],2147483647),number(f["display_height"],2147483647),
                            number(f["channel_span"],2147483647),number(f["selected_channels"],4),number(f["output_width"],32768),number(f["output_height"],32768),unsigned(job.options.threads),unsigned(number(f["source_sample_bytes"],8)),categorical,number(f["source_pixel_bytes"],32),number(f["source_buffer_bytes"],std::numeric_limits<std::uint64_t>::max())};
                        shape.deep_buffer_bytes=number(f["deep_buffer_bytes"],16384ULL*1024*1024);
                        shape.cms_buffer_bytes=number(f["cms_buffer_bytes"],64ULL*1024*1024);
                        check(f["srgb8"].is_boolean(),"worker sRGB8 footprint mismatch");
                        const bool srgb8=f["srgb8"].get<bool>();
                        shape.srgb8=srgb8;
                        check(f["integer_linear"].is_boolean(),"worker integer colour footprint mismatch");
                        const bool integer_linear=f["integer_linear"].get<bool>();shape.integer_linear=integer_linear;
                        check(f["c2_stream"].is_boolean(),"worker C2 footprint mismatch");shape.c2_stream=f["c2_stream"].get<bool>();
#ifndef THUMBGEN_C2_PNG_STREAM
                        check(!shape.c2_stream,"C2 is unavailable in this build");
#endif
                        task.c2_stream=shape.c2_stream;
                        check(!shape.c2_stream || (integer_linear && job.outputs.size()==1 && (shape.source_sample_bytes==1 || shape.source_sample_bytes==2) &&
                            shape.channel_span==shape.selected_channels && shape.data_width==shape.display_width && shape.data_height==shape.display_height),"invalid C2 processing footprint");
                        check(!integer_linear || (!srgb8 && mode=="colour" && job.options.preset=="balanced" &&
                            job.options.display.empty() && !base.contains("deep") && base.contains("source") &&
                            ((shape.source_sample_bytes==1 && base["source"]["sample_type"]=="uint8") ||
                             (shape.source_sample_bytes==2 && base["source"]["sample_type"]=="uint16"))),"invalid integer colour processing plan");
                        check(!srgb8 || (mode=="colour" && job.options.preset=="balanced" &&
                            !base.contains("deep") && base.contains("source") && base["source"]["sample_type"]=="uint8"),"invalid sRGB8 processing plan");
                        check(!shape.cms_buffer_bytes || (mode=="colour" && job.options.input_space=="auto" && !base.contains("deep")),"invalid worker CMS footprint");
                        const auto decode_budget=job.options.max_decode_mib*bounded_mib;
                        shape.source_codec_bytes=number(f["source_codec_bytes"],decode_budget);
                        const auto& bounded=f["bounded_read"];
                        if(!bounded.is_null()){
                            fields(bounded,{"pixel_bytes","retained_bytes","transpose"});
                            check(mode=="colour" && job.options.preset=="balanced" && job.outputs.size()>=1 && job.outputs.size()<=2 &&
                                !shape.c2_stream && !shape.native_categorical && !shape.deep_buffer_bytes && !shape.source_buffer_bytes &&
                                shape.data_width && shape.data_height && shape.data_width<=65536 && shape.data_height<=65536 &&
                                bounded["transpose"].is_boolean(),"invalid bounded reader footprint");
                            const bool transpose=bounded["transpose"].get<bool>();
                            check(shape.display_width==(transpose?shape.data_height:shape.data_width) &&
                                shape.display_height==(transpose?shape.data_width:shape.data_height),"bounded display footprint mismatch");
                            std::vector<std::pair<int,int>> sizes;
                            for(const auto& output:job.outputs)
                                sizes.push_back(fit_preview(int(shape.data_width),int(shape.data_height),
                                    transpose?output.height:output.width,transpose?output.width:output.height));
                            std::size_t workspace=0;
                            check(C2Resize::workspace_plan(int(shape.data_width),int(shape.data_height),int(shape.selected_channels),
                                sizes,workspace,bounded_sampler_limit),
                                "invalid bounded sampler footprint");
                            const auto bytes=number(bounded["pixel_bytes"],32);
                            check(bytes>0,"invalid bounded native pixel size");
                            shape.bounded=bounded_read_memory(shape.data_width,shape.data_height,unsigned(shape.selected_channels),unsigned(bytes),
                                number(bounded["retained_bytes"],decode_budget),shape.source_codec_bytes,shape.cms_buffer_bytes,workspace,sizes);
                            check(bounded_total(shape.bounded)<=decode_budget,"bounded decode footprint exceeds limit");
                        }
                        check(base.contains("deep")?(shape.deep_buffer_bytes==job.options.max_decode_mib*1024ULL*1024ULL && shape.selected_channels==4 && !categorical && !shape.source_buffer_bytes):shape.deep_buffer_bytes==0,"worker Deep budget mismatch");
                        check(shape.source_buffer_bytes?(job.options.visualization=="id" && base.contains("source_read") && base["source_read"]["buffer_bytes"]==shape.source_buffer_bytes):!base.contains("source_read"),"sampled source footprint mismatch");
                        check(shape.data_width && shape.data_height && shape.display_width && shape.display_height && shape.channel_span &&
                            (shape.selected_channels==3 || shape.selected_channels==4) && shape.output_width && shape.output_height && shape.source_sample_bytes &&
                            shape.source_pixel_bytes>=shape.source_sample_bytes && shape.source_pixel_bytes<=shape.source_sample_bytes*std::min<std::uint64_t>(shape.channel_span,mode=="colour" || mode=="cryptomatte"?4:3) &&
                            (mode!="colour" || (srgb8?shape.source_sample_bytes==1:integer_linear?(shape.source_sample_bytes==1 || shape.source_sample_bytes==2):
                             (shape.source_sample_bytes==4 && shape.source_pixel_bytes%4==0))),"invalid worker footprint");
                        std::uint64_t cost=1;
                        try {auto estimate=estimate_memory(shape);estimate_mib=double(estimate.total)/1048576;task.admission_mib=double(estimate.incremental)/1048576;if(options.memory_mib)cost=estimate.incremental;}
                        catch(const std::exception&){if(options.memory_mib)throw FileFailure("task_memory_exceeds_budget","task memory estimate is invalid or overflows");}
                        check(bool(budget),"missing runtime budget");
                        budget->push({job.id,unsigned(job.options.threads),cost});queued=true;task.queued_at=Clock::now();
                    }catch(const FileFailure& e){reply["error"]=error(e);}
                    preparing.reset();
                    if(reply.contains("error")) {channel->send("run",job.id,reply);deadline=Clock::now()+options.startup_timeout;}
                    else deadline=Clock::time_point::max();
                } else if(message.type=="reserve") {
                    check(planned && ran,"output reservation before run");fields(payload,{"output_index"});auto index=size_t(number(payload["output_index"],job.outputs.size()-1));check(reserved.insert(index).second,"duplicate output reservation");
                    Json reply{{"output_index",index}};
                    try {
                        auto file=std::make_unique<TemporaryOutput>(fs::u8path(job.outputs[index].path),job.outputs[index].format);auto id=std::to_string(++temp_serial);
                        reply["temporary_id"]=id;reply["path"]=file->path().u8string();owned.emplace(index,Task::Owned{id,std::move(file)});
                    }catch(const FileFailure& e){reply["error"]=error(e);}
                     catch(const fs::filesystem_error& e){reply["error"]=error(FileFailure("output_failed",error_message(e.code())));}
                    if(reply.contains("error"))ledger[index]=failed_output(job.outputs[index],reply["error"]);
                    channel->send("reserved",job.id,reply);
                } else if(message.type=="encoded") {
                    check(planned && ran,"encoded output before run");fields(payload,{"output_index","temporary_id","encoded"});auto index=size_t(number(payload["output_index"],job.outputs.size()-1));
                    check(owned.count(index) && payload["temporary_id"]==owned.at(index).id && !ledger[index],"unknown or repeated encoded output");
                    const auto& encoded=payload["encoded"];fields(encoded,{"base","output"});base_check(encoded["base"],job);base=encoded["base"];
                    const bool stream_result=base.contains("colour") && base["colour"].value("processing",std::string{})=="integer-linear-stream-v1";
                    check(stream_result==task.c2_stream,"worker C2 processing changed after admission");
                    Json output=encoded["output"];check(output.value("path",std::string{})==job.outputs[index].path && output.value("status",std::string{})=="success" &&
                        output.value("format",std::string{})==job.outputs[index].format && output.value("existing_policy",std::string{})==job.outputs[index].existing,"encoded output identity mismatch");
                    output_check(output,job.outputs[index]);
                    check(number(output["width"],job.outputs[index].width)>0 && number(output["height"],job.outputs[index].height)>0,"encoded output dimensions mismatch");
                    Json reply{{"output_index",index}};
                    try {
                        if(cancelled())throw FileFailure("task_cancelled","controller accepted interruption");
                        if(!source || !source_matches(fs::u8path(job.input),*source))throw FileFailure("source_changed","source changed before controller commit",true);
                        bool committed=owned.at(index).file->commit(fs::u8path(job.outputs[index].path),fs::u8path(job.input),job.outputs[index].existing);
                        if(!committed)output={{"path",job.outputs[index].path},{"existing_policy","skip"},{"status","skipped"},{"reason","output_exists"}};
                        ledger[index]=output;reply["status"]=output["status"];
                    }catch(const FileFailure& e){if(e.code=="task_cancelled")throw;reply["error"]=error(e);ledger[index]=failed_output(job.outputs[index],reply["error"]);if(e.code=="source_changed")base["source_check"]["stable"]=false;}
                    cleanup(index);channel->send("committed",job.id,reply);
                } else if(message.type=="result") {
                    check(!queued,"worker returned a result while awaiting admission");
                    fields(payload,{"record","pin"});result=payload["record"];base_check(result,job);accept_pin(payload["pin"]);
                    if(planned && job.options.visualization=="auto")
                        check(result.value("cryptomatte",Json{})==base.value("cryptomatte",Json{}),"worker automatic preview changed after admission");
                    if(result.contains("colour") && result["colour"].contains("processing"))
                        check((result["colour"]["processing"]=="integer-linear-stream-v1")==task.c2_stream,"worker result processing changed after admission");
                    check(result.contains("outputs") && result["outputs"].is_array(),"invalid worker outputs");
                    auto size=result["outputs"].size();check(size==job.outputs.size() || (size==0 && result.contains("error")),"worker result output count mismatch");
                    check(planned || result.value("status",std::string{})=="failed" || result.value("status",std::string{})=="skipped","worker result bypassed admission");
                    for(size_t i=0;i<size;++i) {
                        const auto& output=result["outputs"][i];check(output.value("path",std::string{})==job.outputs[i].path && output.value("existing_policy",std::string{})==job.outputs[i].existing,"worker output path/policy mismatch");
                        output_check(output,job.outputs[i]);
                        auto status=output.value("status",std::string{});check(status=="failed" || status=="skipped" || status=="success","invalid worker output status");
                        if(status=="success")check(ledger[i] && (*ledger[i])["status"]=="success","worker claimed uncommitted success");
                        if(status=="skipped")check(job.outputs[i].existing=="skip","worker claimed invalid skip");
                        if(ledger[i])check((*ledger[i])["status"]==status,"worker result contradicts commit ledger");
                    }
                    size_t complete=0,skips=0;
                    for(const auto& output:result["outputs"]) {if(output["status"]=="success" || output["status"]=="skipped")++complete;if(output["status"]=="skipped")++skips;}
                    bool ok=size==job.outputs.size() && complete==size;
                    std::string status=ok?(skips==size?"skipped":"success"):(complete?"partial":"failed");
                    check(result.contains("ok") && result["ok"].is_boolean() && result["ok"]==ok && result["status"]==status,"worker task status arithmetic mismatch");
                    if(!planned)check((size==0 && result.contains("error")) || status=="skipped","worker bypassed decode admission");
                    task.finished=true;
                } else throw ProtocolError("unexpected worker message state");
            }catch(const std::exception& e) {
                FileFailure failure("worker_protocol_error",e.what());
                if(auto file=dynamic_cast<const FileFailure*>(&e))failure=*file;
                if(channel) {
                    if(failure.code!="task_cancelled")failure=FileFailure(failure.code,stop_diagnostic(channel->process,failure.what()),failure.retryable);
                    channel.reset();
                }
                if(queued){task.queue_ms=std::chrono::duration<double,std::milli>(Clock::now()-task.queued_at).count();budget->erase_pending(job.id);queued=false;}
                task.finished=true;result=base;result["outputs"]=Json::array();size_t complete=0,skipped=0,cancelled_outputs=0;
                for(size_t i=0;i<job.outputs.size();++i) {
                    if(ledger[i]){result["outputs"].push_back(*ledger[i]);if((*ledger[i])["status"]=="success" || (*ledger[i])["status"]=="skipped")++complete;if((*ledger[i])["status"]=="skipped")++skipped;}
                    else {
                        auto output=failed_output(job.outputs[i],error(failure));
                        if(failure.code=="task_cancelled"){output["status"]="cancelled";++cancelled_outputs;}
                        result["outputs"].push_back(std::move(output));
                    }
                }
                result["ok"]=complete==job.outputs.size();result["status"]=result["ok"].get<bool>()?(skipped==complete?"skipped":"success"):(complete?"partial":"failed");
                if(cancelled_outputs)result["status"]="cancelled";
                result["timings_ms"]["total"]=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
                if(result["ok"].get<bool>())result["warnings"].push_back(clean(std::string(failure.code=="task_cancelled"?"interruption accepted after all outputs settled: ":"worker lost after all outputs committed: ")+failure.what()));
                else {
                    result["error"]=error(failure);
                    if(failure.code=="task_cancelled" && !cancelled_outputs)for(const auto& output:result["outputs"])if(output["status"]=="failed"){result["error"]=output["error"];break;}
                }
            }
            if(!task.finished)continue;
            while(!owned.empty())cleanup(owned.begin()->first);
            if(admitted)budget->release(job.id);
            if(base.contains("warnings"))for(const auto& warning:base["warnings"])if(!result.contains("warnings") || std::find(result["warnings"].begin(),result["warnings"].end(),warning)==result["warnings"].end())result["warnings"].push_back(warning);
            result["execution"]={{"requested_threads",requested_job.options.threads},{"threads",job.options.threads},{"thread_budget",thread_budget},
                {"memory_budget_mib",options.memory_mib},{"fixed_reserve_mib",fixed_reserve},{"wall_ms",std::chrono::duration<double,std::milli>(Clock::now()-started).count()}};
            if(!worker_id.empty())result["execution"]["worker_id"]=worker_id;
            if(estimate_mib){result["execution"]["estimated_task_mib"]=*estimate_mib;result["execution"]["admission_mib"]=*task.admission_mib;result["execution"]["estimate_model"]="buffers-v3";}
            result["execution"]["queue_ms"]=task.queue_ms;
            result["execution"]["workers"]=workers;
#if defined(THUMBGEN_EXR_PROFILE) && THUMBGEN_EXR_PROFILE
            // All manifest jobs are available at this controller entry. This
            // includes undispatched backlog, unlike admission-only queue_ms.
            // Boundary ends before JSON emission, not at process/file flush.
            result["timings_ms"]["controller_batch_to_result_ready_ms"]=
                std::chrono::duration<double,std::milli>(Clock::now()-profile_batch_started).count();
#endif
            emit(result);
            if(!job.sequence_id.empty() && --remaining.at(job.sequence_id)==0) {
                pins.forget_sequence_lock(job.sequence_id);requests.erase(job.sequence_id);remaining.erase(job.sequence_id);
            }
            if(preparing && *preparing==task.index)preparing.reset();
            lane.task.reset();++completed;
            progress=true;

        }
        // A waiting head never gets bypassed, including by smaller prepared tasks.
        if(budget && !cancelled())for(;;) {
            auto admission=budget->take();
            if(admission.state==Admission::State::empty || admission.state==Admission::State::waiting)break;
            auto it=std::find_if(lanes.begin(),lanes.end(),[&](const Lane& lane){return lane.task && lane.task->job.id==admission.task.id;});
            check(it!=lanes.end(),"admitted task has no owning lane");auto& task=*it->task;
            task.queued=false;task.admitted=admission.state==Admission::State::admitted;task.ran=task.admitted;
            task.queue_ms=std::chrono::duration<double,std::milli>(Clock::now()-task.queued_at).count();
            Json reply=Json::object();
            if(!task.admitted)reply["error"]=error(FileFailure("task_memory_exceeds_budget",
                "incremental task "+std::to_string(admission.task.estimated_memory/1048576+(admission.task.estimated_memory%1048576!=0))+
                " MiB + fixed reserve "+std::to_string(fixed_reserve)+" MiB exceeds budget "+std::to_string(options.memory_mib)+" MiB"));
            // A send failure closes this worker; the next poll recovers its ledger.
            try {it->channel->send("run",task.job.id,reply);}
            catch(const std::exception&) {it->channel->process.stop();}
            task.deadline=task.admitted?(options.task_timeout.count()?Clock::now()+options.task_timeout:Clock::time_point::max()):Clock::now()+options.startup_timeout;
            progress=true;
        }
        if(!progress) {
            auto deadline=Clock::time_point::max();
            for(const auto& lane:lanes)if(lane.task)deadline=std::min(deadline,lane.task->deadline);
            // Signal handlers remain atomic-only. Bound cancellation polling
            // without adding a timer thread or changing timer resolution.
            if(options.cancelled)deadline=std::min(deadline,Clock::now()+std::chrono::milliseconds(10));
#if defined(THUMBGEN_TEST_HOOKS)
            if(test_scan_hook)test_scan_hook(wakeup);
#endif
            wakeup->wait(observed,deadline);
        }
    }
    // All result/commit handshakes are complete. Destruction owns shutdown and reaping.
    return cancelled();
}
}
