#include "worker_protocol.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

namespace thumbgen::detail {
namespace {
void require(bool ok,const char* message){if(!ok)throw ProtocolError(message);}
void fields(const Json& object,std::initializer_list<const char*> keys) {
    require(object.is_object() && object.size()==keys.size(),"invalid worker object fields");
    for(auto key:keys)require(object.contains(key),"missing worker object field");
}
std::string text(const Json& value) {require(value.is_string(),"expected worker string");return value.get<std::string>();}
int integer(const Json& value,int minimum,int maximum) {
    require(value.is_number_integer(),"expected worker integer");
    auto number=value.get<long double>();require(number>=minimum && number<=maximum,"worker integer out of range");
    return value.get<int>();
}
void path(const std::string& value) {
    require(!value.empty() && value.find('\0')==std::string::npos && std::filesystem::u8path(value).is_absolute(),"worker path must be absolute without NUL");
}
void member(const std::string& value,std::initializer_list<const char*> options) {
    require(std::any_of(options.begin(),options.end(),[&](const char* option){return value==option;}),"invalid worker enum value");
}
void message(const std::string& type,const std::string& task,const Json& payload) {
    member(type,{"hello","hello_ack","prepare","planned","run","reserve","reserved","encoded","committed","result","shutdown","shutdown_ack"});
    const bool idle=type=="hello" || type=="hello_ack" || type=="shutdown" || type=="shutdown_ack";
    require(idle?task.empty():!task.empty(),"invalid worker task identity");require(payload.is_object(),"worker payload must be an object");
}
}
WorkerSession::WorkerSession(std::string token,std::string worker):token_(std::move(token)),worker_(std::move(worker)) {
    require(token_.size()==32 && std::all_of(token_.begin(),token_.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid worker token");
    require(!worker_.empty() && worker_.size()<=64 && std::all_of(worker_.begin(),worker_.end(),[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}),"invalid worker id");
}
[[noreturn]] void WorkerSession::reject(const std::string& reason){failed_=true;throw ProtocolError(reason);}
std::string WorkerSession::encode(const std::string& type,const std::string& task,const Json& payload) {
    if(failed_)throw ProtocolError("worker session is faulted");
    try {
        message(type,task,payload);require(sent_!=std::numeric_limits<std::uint64_t>::max(),"worker serial exhausted");
        auto frame=Json{{"version",1},{"token",token_},{"worker_id",worker_},{"serial",sent_+1},
            {"type",type},{"task_id",task},{"payload",payload}}.dump(-1,' ',false,Json::error_handler_t::replace);
        require(frame.size()<=max_frame,"worker frame exceeds limit");++sent_;return frame;
    }catch(const std::exception& e){reject(e.what());}
}
WorkerMessage WorkerSession::decode(const std::string& frame) {
    if(failed_)throw ProtocolError("worker session is faulted");
    try {
        require(!frame.empty() && frame.size()<=max_frame && frame.find_first_of("\r\n")==std::string::npos,"invalid worker frame boundary");
        auto value=Json::parse(frame);fields(value,{"version","token","worker_id","serial","type","task_id","payload"});
        require(value["version"].is_number_integer() && value["version"]==1,"worker protocol version mismatch");
        require(text(value["token"])==token_ && text(value["worker_id"])==worker_,"worker association mismatch");
        require(received_!=std::numeric_limits<std::uint64_t>::max() && value["serial"].is_number_unsigned() &&
            value["serial"].get<std::uint64_t>()==received_+1,"worker serial mismatch");
        WorkerMessage result{text(value["type"]),text(value["task_id"]),value["payload"]};message(result.type,result.task_id,result.payload);
        ++received_;return result;
    }catch(const std::exception& e){reject(e.what());}
}
Json job_to_wire(const Job& job) {
    const auto& o=job.options;Json outputs=Json::array();
    for(const auto& out:job.outputs)outputs.push_back({{"path",out.path},{"format",out.format},{"width",out.width},{"height",out.height},{"quality",out.quality},{"existing",out.existing}});
    return {{"id",job.id},{"input",job.input},{"sequence_id",job.sequence_id},{"outputs",outputs},
        {"options",{{"preset",o.preset},{"input_space",o.input_space},{"background",o.background},{"ocio_config",o.ocio_config},
            {"display",o.display},{"view",o.view},{"channels",o.channels},{"part_name",o.part_name},{"selection_policy",o.selection_policy},
            {"part",o.part},{"threads",o.threads},{"max_decode_mib",o.max_decode_mib},
            {"visualization",o.visualization},{"data_range",o.data_range},{"invalid_values",o.invalid_values},{"deep_mode",o.deep_mode},{"deep_samples_per_pixel",o.deep_samples_per_pixel}}}};
}
Job job_from_wire(const Json& value) {
    fields(value,{"id","input","sequence_id","options","outputs"});Job job;
    job.id=text(value["id"]);require(!job.id.empty(),"empty worker job id");job.input=text(value["input"]);path(job.input);job.sequence_id=text(value["sequence_id"]);
    const auto& options=value["options"];fields(options,{"preset","input_space","background","ocio_config","display","view","channels","part_name","selection_policy","part","threads","max_decode_mib","visualization","data_range","invalid_values","deep_mode","deep_samples_per_pixel"});
    auto& o=job.options;
    o.preset=text(options["preset"]);member(o.preset,{"fast","balanced","quality"});o.input_space=text(options["input_space"]);require(!o.input_space.empty(),"empty input space");
    o.background=text(options["background"]);member(o.background,{"black","white","transparent"});o.ocio_config=text(options["ocio_config"]);
    o.display=text(options["display"]);o.view=text(options["view"]);require(o.display.empty()==o.view.empty(),"unpaired display/view");
    o.channels=text(options["channels"]);o.part_name=text(options["part_name"]);o.selection_policy=text(options["selection_policy"]);member(o.selection_policy,{"primary","root_rgb"});
    o.part=integer(options["part"],-1,std::numeric_limits<int>::max());require(o.part<0 || o.part_name.empty(),"conflicting part selectors");
    o.threads=integer(options["threads"],1,256);o.max_decode_mib=size_t(integer(options["max_decode_mib"],1,16384));
    o.visualization=text(options["visualization"]);member(o.visualization,{"colour","depth","normal","vector","id","id_rgb","cryptomatte"});
    o.deep_mode=text(options["deep_mode"]);member(o.deep_mode,{"reject","flatten"});
    o.deep_samples_per_pixel=integer(options["deep_samples_per_pixel"],1,1048576);
    require(o.deep_mode!="flatten" || o.visualization=="colour","Deep flattening requires colour visualization");
    o.invalid_values=text(options["invalid_values"]);member(o.invalid_values,{"black","magenta","error"});
    require(options["data_range"].is_array() && (options["data_range"].empty() || options["data_range"].size()==2),"invalid worker data range");
    for(const auto& endpoint:options["data_range"]) {require(endpoint.is_number() && std::isfinite(endpoint.get<double>()),"invalid worker range number");o.data_range.push_back(endpoint.get<double>());}
    if(!o.data_range.empty())require(o.data_range[1]>o.data_range[0] && std::isfinite(o.data_range[1]-o.data_range[0]),"invalid worker range extent");
    if(o.visualization=="colour" || o.visualization=="id" || o.visualization=="cryptomatte")require(o.data_range.empty(),"range not supported by visualization");
    if(o.visualization=="depth" || o.visualization=="vector" || o.visualization=="id_rgb")require(o.data_range.size()==2,"missing fixed data range");
    if(o.visualization=="colour")require(o.invalid_values=="magenta","data value policy without data mode");
    else require(!o.channels.empty() && o.input_space=="auto" && o.ocio_config.empty() && o.display.empty() && o.view.empty(),"invalid data selection/colour options");
    require(value["outputs"].is_array() && !value["outputs"].empty(),"empty worker output list");std::set<std::string> paths;
    for(const auto& output:value["outputs"]) {
        fields(output,{"path","format","width","height","quality","existing"});Output out;out.path=text(output["path"]);path(out.path);
        require(paths.insert(out.path).second && out.path!=job.input,"duplicate or aliased worker output path");
        out.format=text(output["format"]);member(out.format,{"jpeg","png","webp"});out.width=integer(output["width"],1,32768);out.height=integer(output["height"],1,32768);
        out.quality=integer(output["quality"],1,100);out.existing=text(output["existing"]);member(out.existing,{"error","skip","replace"});
        require(out.format!="jpeg" || o.background!="transparent","JPEG cannot preserve alpha");job.outputs.push_back(std::move(out));
    }
    return job;
}
}
