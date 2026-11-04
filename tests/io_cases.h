#pragma once
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

static Json io_case(const fs::path& root,const std::string& name) {
    auto input=root/"source.exr",target=root/"preview.png";
    ImageSpec spec(4,2,3,TypeDesc::FLOAT);
    write(input,spec,std::vector<float>(24,0.4f));
    const auto original=bytes(input);
    auto save=[](const fs::path& path,const std::string& content){std::ofstream stream(path,std::ios::binary);stream<<content;require(bool(stream),"fixture save failed");};
    thumbgen::Job job;job.id=name;job.input=input.u8string();job.options.input_space="linear";
    job.outputs.push_back({target.u8string(),"png",4,2,90});
    Json records=Json::array();
    auto run=[&](thumbgen::Engine::Observer observer={}) {
        thumbgen::Engine engine(std::move(observer));auto result=engine.convert(job);records.push_back(result);return result;
    };
    auto failure=[](const Json& result,const std::string& code) {
        require(!result["ok"].get<bool>(),result.dump());
        const auto& error=result.contains("error")?result["error"]:result["outputs"][0]["error"];
        require(error["code"]==code,result.dump());
    };
    bool expect_source_unchanged=true,foreign=false;
    if(name=="io_error" || name=="io_skip" || name=="io_replace") {
        save(target,"old-target");
        job.outputs[0].existing=name=="io_skip"?"skip":name=="io_replace"?"replace":"error";
        if(name=="io_skip")job.options.max_decode_mib=0; // Proves no pixel buffer is decoded for an all-skip task.
        auto result=run();
        if(name=="io_error")failure(result,"output_exists");
        else require(result["ok"].get<bool>(),result.dump());
        if(name=="io_skip")require(result["status"]=="skipped" && !result["timings_ms"].contains("decode"),result.dump());
        if(name!="io_replace")require(bytes(target)=="old-target","existing target changed");
        else {OIIO::ImageBuf image(target.u8string());require(image.read(),image.geterror());}
    } else if(name=="io_partial") {
        save(root/"blocked","keep-parent");
        job.outputs.push_back({(root/"blocked/second.png").u8string(),"png",4,2,90});
        auto result=run();require(result["status"]=="partial" && result["outputs"][0]["status"]=="success" && result["outputs"][1]["status"]=="failed",result.dump());
        require(bytes(root/"blocked")=="keep-parent" && fs::exists(target),"partial output lost");
    } else if(name=="io_hardlink") {
        fs::create_hard_link(input,target);job.outputs[0].existing="replace";
        failure(run(),"output_aliases_input");require(bytes(target)==original,"hard-linked input overwritten");
    } else if(name=="io_race_error" || name=="io_race_skip" || name=="io_race_replace") {
        job.outputs[0].existing=name=="io_race_skip"?"skip":name=="io_race_replace"?"replace":"error";
        auto result=run([&](const std::string& phase,const thumbgen::Job&){if(phase=="encoded")save(target,"competing-writer");});
        if(name=="io_race_error")failure(result,"output_exists");
        else if(name=="io_race_skip")require(result["status"]=="skipped",result.dump());
        else require(result["ok"].get<bool>(),result.dump());
        if(name!="io_race_replace")require(bytes(target)=="competing-writer","race replaced another writer's output");
        else {OIIO::ImageBuf image(target.u8string());require(image.read(),image.geterror());}
    } else if(name=="io_source_decode" || name=="io_source_identity" || name=="io_source_commit" || name=="io_source_partial") {
        expect_source_unchanged=false;
        if(name=="io_source_commit") {save(target,"keep-on-source-change");job.outputs[0].existing="replace";}
        if(name=="io_source_partial") {
            job.outputs.push_back({(root/"second.png").u8string(),"png",4,2,90});
            job.outputs.push_back({(root/"third.png").u8string(),"png",4,2,90});
        }
        int encoded=0;
        auto result=run([&](const std::string& phase,const thumbgen::Job&) {
            if(phase=="encoded")++encoded;
            const bool decoded=name=="io_source_decode" || name=="io_source_identity";
            if((decoded && phase=="decoded") || (!decoded && phase=="encoded" && encoded==(name=="io_source_partial"?2:1))) {
                if(name=="io_source_identity") {
                    auto modified=fs::last_write_time(input);
                    fs::copy_file(input,root/"replacement.exr");fs::remove(input);fs::rename(root/"replacement.exr",input);
                    fs::last_write_time(input,modified); // Same bytes/size/mtime; only file identity changes.
                } else {std::ofstream stream(input,std::ios::binary|std::ios::app);stream<<"changed";}
            }
        });
        require(result["source_check"]["stable"]==false,result.dump());
        if(name=="io_source_partial") {
            require(result["status"]=="partial" && result["outputs"][0]["status"]=="success",result.dump());
            require(result["outputs"][1]["error"]["code"]=="source_changed" && result["outputs"][1]["error"]["retryable"]==true,result.dump());
            require(fs::exists(target) && !fs::exists(root/"second.png") && !fs::exists(root/"third.png"),"source-change partial commit mismatch");
        } else {
            failure(result,"source_changed");
            if(name=="io_source_commit")require(bytes(target)=="keep-on-source-change","replace committed changed input");
            else require(!fs::exists(target),"changed source produced an output");
        }
    } else if(name=="io_cleanup_foreign") {
        foreign=true;
        auto result=run([&](const std::string& phase,const thumbgen::Job&) {
            if(phase=="encoded")for(const auto& entry:fs::directory_iterator(root))
                if(entry.path().filename().u8string().rfind(".thumbgen-tmp-",0)==0)save(entry.path()/"foreign.txt","do-not-delete");
        });
        require(result["ok"].get<bool>() && !result["warnings"].empty(),result.dump());
        int count=0;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.path().filename()=="foreign.txt"){++count;require(bytes(entry.path())=="do-not-delete","foreign file changed");}
        require(count==1,"cleanup deleted an unrelated file");
    }
#ifndef _WIN32
    else if(name=="io_symlink") {
        save(root/"linked.png","leave-linked-target");fs::create_symlink(root/"linked.png",target);
        job.outputs[0].existing="replace";failure(run(),"output_symlink");
        require(bytes(root/"linked.png")=="leave-linked-target" && fs::is_symlink(target),"symlink target changed");
    } else if(name=="io_write_limit") {
        job.outputs.clear();
        for(const auto& format:{"png","jpeg","webp"}) {
            auto path=root/(std::string("limited.")+format);save(path,"keep-on-write-failure");
            job.outputs.push_back({path.u8string(),format,4,2,90,"replace"});
        }
        struct rlimit previous{};require(getrlimit(RLIMIT_FSIZE,&previous)==0,"getrlimit failed");
        auto handler=std::signal(SIGXFSZ,SIG_IGN);
        struct Restore {struct rlimit limit;decltype(handler) signal;~Restore(){setrlimit(RLIMIT_FSIZE,&limit);std::signal(SIGXFSZ,signal);}} restore{previous,handler};
        auto limited=previous;limited.rlim_cur=16;require(setrlimit(RLIMIT_FSIZE,&limited)==0,"setrlimit failed");
        auto result=run();failure(result,"encode_failed");
        for(size_t i=0;i<job.outputs.size();++i) {
            require(result["outputs"][i]["error"]["code"]=="encode_failed",result.dump());
            require(bytes(fs::u8path(job.outputs[i].path))=="keep-on-write-failure","write failure replaced existing output");
        }
    } else if(name=="io_permission") {
        auto directory=root/"read-only";fs::create_directory(directory);
        struct RestorePermissions {fs::path path;~RestorePermissions(){std::error_code ec;fs::permissions(path,fs::perms::owner_all,ec);}} restore{directory};
        fs::permissions(directory,fs::perms::owner_read|fs::perms::owner_exec);
        job.outputs[0].path=(directory/"cannot-write.png").u8string();
        failure(run(),"output_failed");require(!fs::exists(directory/"cannot-write.png"),"permission failure wrote an output");
    }
#endif
    else throw std::runtime_error("unknown IO case");
    if(expect_source_unchanged)require(bytes(input)==original,"source bytes changed");
    if(!foreign)for(const auto& entry:fs::directory_iterator(root))
        require(entry.path().filename().u8string().rfind(".thumbgen-tmp-",0)!=0,"owned temporary directory leaked");
    return {{"records",records}};
}
