#include <thumbgen/core.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstdlib>
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    std::ifstream input(argv[1]);thumbgen::Json manifest;input>>manifest;
    const std::filesystem::path directory=argv[2];std::filesystem::create_directories(directory);
    const std::string mode=argv[3];
    thumbgen::Engine engine;
    thumbgen::Engine::Control control;control.reference_colour=mode=="product-reference";engine.set_control(control);
    for(const auto& item:manifest.at("cases")) {
        if(mode=="inspect"){auto result=engine.inspect(item.at("input"));result["id"]=item.at("id");std::cout<<result.dump()<<std::endl;continue;}
        thumbgen::Job job;job.id=item.at("id");job.input=item.at("input");
        job.options.threads=1;job.options.max_decode_mib=2048;job.options.ocio_config="ocio://default";
        job.options.background=item.value("background",std::string("black"));
        job.options.input_space=item.value("input_space",std::string("auto"));
        for(const auto& size:manifest.at("sizes")) {
            thumbgen::Output output;output.width=size[0];output.height=size[1];output.format=manifest.value("format",std::string("png"));output.quality=90;
            output.path=(directory/(job.id+"-"+std::to_string(output.width)+"."+(output.format=="jpeg"?"jpg":output.format))).u8string();output.existing="replace";job.outputs.push_back(output);
        }
        try{std::cout<<engine.convert(job).dump()<<std::endl;}
        catch(const std::exception& e){std::cout<<thumbgen::Json{{"id",job.id},{"ok",false},{"error",e.what()}}.dump()<<std::endl;}
    }
}
