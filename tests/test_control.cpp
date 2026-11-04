#include <thumbgen/core.h>
#include "files.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace fs=std::filesystem;
using thumbgen::Json;
using thumbgen::Engine;
using thumbgen::Job;
using thumbgen::detail::TemporaryOutput;
using thumbgen::detail::FileFailure;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static std::string bytes(const fs::path& path){std::ifstream file(path,std::ios::binary);require(bool(file),"snapshot failed");return {std::istreambuf_iterator<char>(file),{}};}
static void save(const fs::path& path,const std::string& value){std::ofstream file(path,std::ios::binary);file<<value;require(bool(file),"save failed");}
static void write(const fs::path& path,OIIO::ImageSpec spec,std::vector<float> pixels) {
    auto writer=OIIO::ImageOutput::create(path.u8string());require(bool(writer),OIIO::geterror());
    require(writer->open(path.u8string(),spec),writer->geterror());
    require(writer->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),writer->geterror());require(writer->close(),writer->geterror());
}
static Job fixture(const fs::path& directory,const std::string& id) {
    fs::create_directory(directory);Job job;job.id=id;job.input=(directory/"source.exr").u8string();
    write(fs::u8path(job.input),OIIO::ImageSpec(4,2,3,OIIO::TypeDesc::FLOAT),std::vector<float>(24,0.4f));
    job.options.input_space="linear";
    job.outputs={{(directory/"first.png").u8string(),"png",4,2,90},{(directory/"second.png").u8string(),"png",2,2,90}};
    return job;
}
struct Controller {
    std::map<size_t,std::unique_ptr<TemporaryOutput>> owned;
    Json plan;
    int commits=0;
    std::function<void(size_t)> before_commit;
    Engine::Control hooks() {
        Engine::Control control;
        control.planned=[this](const Job&,const Json& value){plan=value;};
        control.reserve=[this](const Job& job,size_t index) {
            require(!owned.count(index),"duplicate temporary reservation");
            auto temp=std::make_unique<TemporaryOutput>(fs::u8path(job.outputs[index].path),job.outputs[index].format);
            auto path=temp->path().u8string();owned.emplace(index,std::move(temp));return path;
        };
        control.commit=[this](const Job& job,size_t index,const std::string& path,const Json& encoded) {
            require(owned.count(index) && owned.at(index)->path()==fs::u8path(path),"unknown borrowed temporary");
            require(encoded["base"]["id"]==job.id && encoded["output"]["status"]=="success" && encoded["base"].contains("colour"),encoded.dump());
            OIIO::ImageBuf preview(path);require(preview.read(),preview.geterror());
            require(preview.spec().width==encoded["output"]["width"] && preview.spec().height==encoded["output"]["height"],"encoded metadata mismatch");
            if(before_commit)before_commit(index);
            bool result=owned.at(index)->commit(fs::u8path(job.outputs[index].path),fs::u8path(job.input),job.outputs[index].existing);
            require(owned.at(index)->cleanup().empty(),"controller cleanup failed");owned.erase(index);++commits;return result;
        };
        return control;
    }
};
template<class Exception,class Fn> static void rejects(Fn fn){try{fn();}catch(const Exception&){return;}throw std::runtime_error("invalid control state accepted");}
int main() {
    auto root=fs::temp_directory_path()/("thumbgen-control-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {fs::path root;~Cleanup(){std::error_code ec;for(const auto& directory:fs::directory_iterator(root,ec)) {
        for(const auto& file:fs::directory_iterator(directory.path(),ec))fs::remove(file.path(),ec);
        fs::remove(directory.path(),ec);
    }fs::remove(root,ec);}} cleanup{root};
    Json records=Json::array();
    try {
        {
            auto job=fixture(root/"external","external");const auto original=bytes(fs::u8path(job.input));Controller controller;bool decoded=false;
            Engine engine([&](const std::string& phase,const Job&){if(phase=="decoded")decoded=true;});
            auto control=controller.hooks();auto planned=control.planned;
            control.planned=[&](const Job& value,const Json& plan){require(!decoded && !plan["base"].contains("colour"),"plan occurred after allocation/colour");planned(value,plan);
                rejects<std::logic_error>([&]{engine.set_control({});});};
            engine.set_control(std::move(control));auto result=engine.convert(job);records.push_back(result);
            require(result["ok"].get<bool>() && decoded && controller.commits==2 && controller.owned.empty(),result.dump());
            require(controller.plan["footprint"]["channel_span"]==3 && controller.plan["footprint"]["output_width"]==4,controller.plan.dump());
            require(controller.plan["source_stamp"]["size"]==fs::file_size(fs::u8path(job.input)),"source stamp missing");
            require(bytes(fs::u8path(job.input))==original,"source changed");
            Engine::Control unpaired;unpaired.reserve=[](const Job&,size_t){return std::string{};};
            rejects<std::invalid_argument>([&]{engine.set_control(unpaired);});
        }
        {
            auto job=fixture(root/"refused","refused");job.options.ocio_config="missing-control-test.ocio";bool decoded=false,planned=false;
            Engine engine([&](const std::string& phase,const Job&){if(phase=="decoded")decoded=true;});Engine::Control control;
            control.planned=[&](const Job&,const Json&){planned=true;throw std::runtime_error("controller refused admission");};
            engine.set_control(control);auto result=engine.convert(job);records.push_back(result);
            require(planned && !decoded && result["error"]["code"]=="conversion_failed" && !fs::exists(fs::u8path(job.outputs[0].path)),result.dump());
        }
        {
            auto job=fixture(root/"waiting-source","waiting-source");bool decoded=false;Engine engine([&](const std::string& phase,const Job&){if(phase=="decoded")decoded=true;});Engine::Control control;
            control.planned=[&](const Job&,const Json&){std::ofstream file(fs::u8path(job.input),std::ios::binary|std::ios::app);file<<"changed while waiting";};
            engine.set_control(control);auto result=engine.convert(job);records.push_back(result);
            require(!decoded && result["error"]["code"]=="source_changed" && result["source_check"]["stable"]==false,result.dump());
        }
        {
            auto job=fixture(root/"skip","skip");job.sequence_id="skip-sequence";job.options.max_decode_mib=0;
            for(auto& output:job.outputs){output.existing="skip";save(fs::u8path(output.path),"existing");}
            Engine engine;Engine::Control control;control.planned=[](const Job&,const Json&){throw std::runtime_error("skip requested decode admission");};engine.set_control(control);
            auto result=engine.convert(job);records.push_back(result);require(result["status"]=="skipped" && !engine.sequence_lock(job.sequence_id).is_null(),result.dump());
        }
        {
            auto job=fixture(root/"partial","partial");job.outputs[1].existing="replace";save(fs::u8path(job.outputs[1].path),"keep-second");
            auto stamp=thumbgen::detail::source_stamp(fs::u8path(job.input));Controller controller;
            controller.before_commit=[&](size_t index){if(index==1){std::ofstream file(fs::u8path(job.input),std::ios::app|std::ios::binary);file<<"changed";file.close();}
                if(!thumbgen::detail::source_matches(fs::u8path(job.input),stamp))throw FileFailure("source_changed","controller source guard",true);};
            Engine engine;engine.set_control(controller.hooks());auto result=engine.convert(job);records.push_back(result);
            require(result["status"]=="partial" && result["outputs"][0]["status"]=="success" && result["outputs"][1]["error"]["code"]=="source_changed" && result["source_check"]["stable"]==false,result.dump());
            require(bytes(fs::u8path(job.outputs[1].path))=="keep-second" && fs::exists(controller.owned.at(1)->path()),"borrowed temporary or old target was deleted");
            require(controller.owned.at(1)->cleanup().empty(),"partial cleanup failed");controller.owned.clear();
        }
        {
            auto job=fixture(root/"aliases","aliases");job.outputs.resize(1);auto original=bytes(fs::u8path(job.input));Engine engine;
            Engine::Control control;control.reserve=[](const Job& value,size_t){return value.input;};control.commit=[](const Job&,size_t,const std::string&,const Json&){return true;};
            engine.set_control(control);auto result=engine.convert(job);records.push_back(result);require(result["outputs"][0]["error"]["code"]=="output_aliases_input",result.dump());
            control.reserve=[](const Job& value,size_t index){return value.outputs[index].path;};engine.set_control(control);result=engine.convert(job);records.push_back(result);
            require(result["outputs"][0]["error"]["code"]=="temporary_create_failed" && !fs::exists(fs::u8path(job.outputs[0].path)) && bytes(fs::u8path(job.input))==original,result.dump());
            Controller controller;control=controller.hooks();control.commit=[](const Job&,size_t,const std::string&,const Json&){return false;};
            engine.set_control(control);result=engine.convert(job);records.push_back(result);
            require(result["outputs"][0]["error"]["code"]=="commit_failed" && fs::exists(controller.owned.at(0)->path()) && !fs::exists(fs::u8path(job.outputs[0].path)),result.dump());
            require(controller.owned.at(0)->cleanup().empty(),"rejected commit cleanup failed");controller.owned.clear();
        }
        {
            auto first=fixture(root/"sequence-first","first");first.sequence_id="sequence";first.outputs.resize(1);
            write(fs::u8path(first.input),OIIO::ImageSpec(4,2,3,OIIO::TypeDesc::FLOAT),std::vector<float>(24,0.0f));
            Engine old_worker;auto result=old_worker.convert(first);records.push_back(result);auto pin=old_worker.sequence_lock(first.sequence_id);
            auto next=fixture(root/"sequence-next","next");next.sequence_id=first.sequence_id;next.outputs.resize(1);
            OIIO::ImageSpec spec(4,2,6,OIIO::TypeDesc::FLOAT);spec.channelnames={"R","G","B","Beauty.R","Beauty.G","Beauty.B"};
            std::vector<float> pixels(48,0.0f);for(size_t i=0;i<pixels.size();++i)if(i%6>=3)pixels[i]=0.8f;write(fs::u8path(next.input),spec,pixels);
            Engine replacement;replacement.restore_sequence_lock(next.sequence_id,pin);result=replacement.convert(next);records.push_back(result);
            require(result["ok"].get<bool>() && result["selection"]["chosen"]["group"]=="RGB" && result["selection"]["lock_status"]=="reused",result.dump());
            OIIO::ImageBuf preview(next.outputs[0].path);require(preview.read(),preview.geterror());float rgb[3];preview.getpixel(0,0,rgb,3);require(rgb[0]==0 && rgb[1]==0 && rgb[2]==0,"new worker upgraded black root to Beauty");
            auto invalid=pin;invalid["named"]=true;rejects<std::invalid_argument>([&]{replacement.restore_sequence_lock("invalid",invalid);});
            invalid=pin;invalid["request"]["strategy"]="guess";rejects<std::invalid_argument>([&]{replacement.restore_sequence_lock("invalid",invalid);});
            invalid=pin;invalid["key"]["kind"]="data";rejects<std::invalid_argument>([&]{replacement.restore_sequence_lock("invalid",invalid);});
            invalid=pin;invalid["key"]["group"]="Beauty";rejects<std::invalid_argument>([&]{replacement.restore_sequence_lock(next.sequence_id,invalid);});
            rejects<std::invalid_argument>([&]{replacement.restore_sequence_lock("",pin);});require(replacement.sequence_lock(next.sequence_id)==pin,"invalid import changed pin");
            auto changed=fixture(root/"sequence-alpha","alpha-change");changed.sequence_id=next.sequence_id;changed.outputs.resize(1);
            OIIO::ImageSpec rgba(4,2,4,OIIO::TypeDesc::FLOAT);rgba.alpha_channel=3;write(fs::u8path(changed.input),rgba,std::vector<float>(32,1.0f));
            result=replacement.convert(changed);records.push_back(result);require(result["error"]["code"]=="sequence_selection_changed",result.dump());
        }
        {
            Json pin;
            for(int frame=0;frame<2;++frame) {
                auto job=fixture(root/("named-"+std::to_string(frame)),"named-"+std::to_string(frame));job.sequence_id="named";job.options.part_name="hero";job.outputs.resize(1);
                OIIO::ImageSpec hero(4,2,3,OIIO::TypeDesc::FLOAT),aux=hero;hero.attribute("oiio:subimagename","hero");aux.attribute("oiio:subimagename","aux");
                OIIO::ImageSpec parts[]{frame?aux:hero,frame?hero:aux};auto out=OIIO::ImageOutput::create(job.input);require(bool(out),OIIO::geterror());
                require(out->open(job.input,2,parts),out->geterror());std::vector<float> values(24,0.4f);require(out->write_image(OIIO::TypeDesc::FLOAT,values.data()),"first named part");
                require(out->open(job.input,parts[1],OIIO::ImageOutput::AppendSubimage),out->geterror());require(out->write_image(OIIO::TypeDesc::FLOAT,values.data()),"second named part");require(out->close(),"named close");
                Engine worker;if(frame)worker.restore_sequence_lock(job.sequence_id,pin);auto result=worker.convert(job);records.push_back(result);
                require(result["ok"].get<bool>() && result["source"]["part"]==frame,result.dump());pin=worker.sequence_lock(job.sequence_id);
                require(pin["named"]==true && !pin["key"].contains("part"),pin.dump());
            }
        }
        {
            auto job=fixture(root/"footprint","footprint");job.outputs[0].width=4;job.outputs[0].height=2;job.outputs[1].width=1024;job.outputs[1].height=1024;
            OIIO::ImageSpec spec(4,2,8,OIIO::TypeDesc::FLOAT);spec.channelnames={"Beauty.R","AA.R","AA.G","Beauty.G","AA.B","AA.Z","Beauty.B","A"};spec.alpha_channel=7;
            spec.full_width=8;spec.full_height=4;spec.attribute("Orientation",6);std::vector<float> pixels(64,0.25f);for(size_t i=7;i<pixels.size();i+=8)pixels[i]=1;
            write(fs::u8path(job.input),spec,pixels);Controller controller;Engine engine;engine.set_control(controller.hooks());auto result=engine.convert(job);records.push_back(result);
            require(result["ok"].get<bool>(),result.dump());auto shape=controller.plan["footprint"];
            require(shape["data_width"]==4 && shape["channel_span"]==8 && shape["selected_channels"]==4 && shape["display_width"]==4 && shape["display_height"]==8 && shape["output_width"]==4 && shape["output_height"]==8,shape.dump());
        }
        std::cout<<Json{{"records",records},{"ok",true}}.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
