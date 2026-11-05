#include "worker_protocol.h"
#include <filesystem>
#include <iostream>

using namespace thumbgen::detail;
using thumbgen::Json;
static void require(bool ok){if(!ok)throw std::runtime_error("worker protocol assertion failed");}
template<class Fn> static void rejects(Fn fn){try{fn();}catch(const ProtocolError&){return;}throw std::runtime_error("bad protocol was accepted");}
int main() {
    try {
        const std::string token(32,'a');WorkerSession parent(token,"w0"),child(token,"w0");
        auto hello=parent.encode("hello","",Json::object());require(child.decode(hello).type=="hello");
        auto ack=child.encode("hello_ack","",{{"threads",4}});require(parent.decode(ack).payload["threads"]==4);
        rejects([&]{child.decode(hello);});require(child.failed());rejects([&]{child.encode("hello_ack","",Json::object());});
        const auto valid=Json::parse(hello);
        for(const auto& key:{"version","token","worker_id","serial","type","task_id","payload","extra"}) {
            auto invalid=valid;
            if(std::string(key)=="version")invalid[key]=2;
            else if(std::string(key)=="serial")invalid[key]=1.0;
            else if(std::string(key)=="payload")invalid[key]=Json::array();
            else invalid[key]="wrong";
            WorkerSession receiver(token,"w0");rejects([&]{receiver.decode(invalid.dump());});require(receiver.failed());
        }
        for(const auto& invalid:{std::string("{"),std::string("\xff"),hello+"\n",std::string(WorkerSession::max_frame+1,'x')}) {
            WorkerSession receiver(token,"w0");rejects([&]{receiver.decode(invalid);});
        }
        thumbgen::Job job;job.id=u8"任务 / 1";auto root=std::filesystem::temp_directory_path();job.input=(root/std::filesystem::u8path(u8"source 空格.exr")).u8string();
        job.sequence_id="sequence";job.options.part_name="hero";job.outputs.push_back({(root/"preview.png").u8string(),"png",512,256,90,"replace"});
        auto wire=job_to_wire(job);require(job_to_wire(job_from_wire(wire))==wire);
        for(const char* field:{"threads","part","max_decode_mib","deep_samples_per_pixel"}) {
            auto bad=wire;bad["options"][field]=1.5;rejects([&]{job_from_wire(bad);});
            bad["options"][field]=18446744073709551615ULL;rejects([&]{job_from_wire(bad);});
        }
        auto bad=wire;bad["outputs"][0]["width"]=0;rejects([&]{job_from_wire(bad);});
        bad=wire;bad["input"]="relative.exr";rejects([&]{job_from_wire(bad);});
        bad=wire;bad["options"]["extra"]=true;rejects([&]{job_from_wire(bad);});
        bad=wire;bad["outputs"].push_back(bad["outputs"][0]);rejects([&]{job_from_wire(bad);});
        job.options.deep_mode="flatten";job.options.deep_samples_per_pixel=17;
        wire=job_to_wire(job);require(job_to_wire(job_from_wire(wire))==wire);
        bad=wire;bad["options"]["deep_mode"]="automatic";rejects([&]{job_from_wire(bad);});
        bad=wire;bad["options"]["deep_samples_per_pixel"]=0;rejects([&]{job_from_wire(bad);});
        bad=wire;bad["options"]["visualization"]="normal";rejects([&]{job_from_wire(bad);});
        job.options.deep_mode="reject";
        job.options.visualization="vector";job.options.channels="motion";job.options.data_range={-2,2};job.options.invalid_values="error";
        wire=job_to_wire(job);require(job_to_wire(job_from_wire(wire))==wire);
        for(const auto& range:{Json::array(),Json::array({0}),Json::array({1,1}),Json::array({2,1}),Json::array({true,1}),Json::array({-1e308,1e308})}) {
            bad=wire;bad["options"]["data_range"]=range;rejects([&]{job_from_wire(bad);});
        }
        bad=wire;bad["options"]["visualization"]="unknown";rejects([&]{job_from_wire(bad);});
        bad=wire;bad["options"]["input_space"]="linear";rejects([&]{job_from_wire(bad);});
        bad=wire;bad["options"]["channels"]="";rejects([&]{job_from_wire(bad);});
        std::cout<<"worker protocol: association, serials, bounds, types and Job roundtrip passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
