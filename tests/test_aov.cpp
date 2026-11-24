#include <thumbgen/core.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/half.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fs=std::filesystem;
using thumbgen::Json;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
template<class T> static void write(const fs::path& path,OIIO::ImageSpec spec,OIIO::TypeDesc type,const std::vector<T>& pixels) {
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror());
    require(output->open(path.u8string(),spec),output->geterror());
    require(output->write_image(type,pixels.data()),output->geterror());require(output->close(),output->geterror());
}
static OIIO::ImageSpec colour_part(const char* name,std::vector<std::string> channels) {
    OIIO::ImageSpec spec(4,2,int(channels.size()),OIIO::TypeDesc::FLOAT);
    spec.channelnames=std::move(channels);
    spec.attribute("oiio:subimagename",name);
    return spec;
}
static void selection_fixtures(const fs::path& root,Json& files) {
    const auto rgb=colour_part("main",{"R","G","B","A","Z","AO.R","AO.G","AO.B",
        "diffuse_indirect.R","diffuse_indirect.G","diffuse_indirect.B"});
    const auto shadow=colour_part("sHaDoW",{"R","G","B"});
    const auto passes=colour_part("passes",{"Fresnel.R","Fresnel.G","Fresnel.B","Normal.R","Normal.G","Normal.B"});
    const auto alternate=colour_part("alternate",{"R","G","B"});
    const auto beauty=colour_part("Beauty",{"R","G","B"});
    auto save=[&](const char* name,std::vector<OIIO::ImageSpec> parts) {
        auto path=(root/name).u8string();
        auto output=OIIO::ImageOutput::create(path);require(bool(output),OIIO::geterror());
        require(output->open(path,int(parts.size()),parts.data()),output->geterror());
        for(std::size_t part=0;part<parts.size();++part) {
            if(part)require(output->open(path,parts[part],OIIO::ImageOutput::AppendSubimage),output->geterror());
            std::vector<float> pixels(8*parts[part].nchannels,.5f);
            require(output->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),output->geterror());
        }
        require(output->close(),output->geterror());
        files[name]=path;
    };
    save("main-first.exr",{rgb,shadow,passes});
    save("main-last.exr",{passes,shadow,rgb});
    save("named-primary.exr",{rgb,shadow,beauty});
    save("two-roots.exr",{rgb,alternate,shadow});
    save("root-with-light-groups.exr",{colour_part("main",{"R","G","B","diffuse_direct.R","diffuse_direct.G","diffuse_direct.B"})});
    save("root-with-primary.exr",{colour_part("main",{"R","G","B","Beauty.R","Beauty.G","Beauty.B"})});
    save("other-view.exr",{rgb,colour_part("views",{"ShadowCamera.R","ShadowCamera.G","ShadowCamera.B"})});
    save("two-views.exr",{colour_part("views",{"left.R","left.G","left.B","right.R","right.G","right.B"})});
    save("prefixed-rgb.exr",{colour_part("main",{"RGB.R","RGB.G","RGB.B"}),shadow});
}
static Json fixtures(const fs::path& root) {
    require(fs::create_directory(root),"fixture directory exists");
    OIIO::ImageSpec depth(4,2,1,OIIO::TypeDesc::FLOAT);depth.channelnames={"Z"};
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    write(root/"depth.exr",depth,OIIO::TypeDesc::FLOAT,std::vector<float>{0,5,10,15,-5,nan,inf,-inf});
    write(root/"depthgood.exr",depth,OIIO::TypeDesc::FLOAT,std::vector<float>{0,5,10,10,0,5,10,10});
    depth.set_format(OIIO::TypeDesc::UINT16);
    write(root/"depth16.tif",depth,OIIO::TypeDesc::UINT16,std::vector<std::uint16_t>{0,5,10,15,0,5,10,15});
    OIIO::ImageSpec normal(4,2,4,OIIO::TypeDesc::FLOAT);normal.channelnames={"N.X","N.Y","N.Z","N.A"};
    normal.channelformats={OIIO::TypeDesc::HALF,OIIO::TypeDesc::FLOAT,OIIO::TypeDesc::UINT32,OIIO::TypeDesc::FLOAT};
    std::vector<unsigned char> packed(8*14);
    for(int p=0;p<8;++p){half x(-1.0f);float y=0,a=0;std::uint32_t z=1;auto* pixel=packed.data()+p*14;
        std::memcpy(pixel,&x,2);std::memcpy(pixel+2,&y,4);std::memcpy(pixel+6,&z,4);std::memcpy(pixel+10,&a,4);}
    write(root/"normal.exr",normal,OIIO::TypeDesc::UNKNOWN,packed);
    OIIO::ImageSpec vector(4,2,2,OIIO::TypeDesc::FLOAT);vector.channelnames={"motion.X","motion.Y"};
    std::vector<float> xy(16);for(int p=0;p<8;++p){xy[p*2]=-2;xy[p*2+1]=2;}
    write(root/"vector.exr",vector,OIIO::TypeDesc::FLOAT,xy);
    OIIO::ImageSpec id(8,4,1,OIIO::TypeDesc::UINT32);id.channelnames={"object_id"};
    std::vector<std::uint32_t> ids;for(int y=0;y<4;++y)for(auto value:{0u,1u,42u,16777216u,16777217u,4294967295u,0u,42u})ids.push_back(value);
    write(root/"ids.exr",id,OIIO::TypeDesc::UINT32,ids);
    id.width=id.full_width=4;id.height=id.full_height=2;id.set_format(OIIO::TypeDesc::DOUBLE);
    write(root/"idsdouble.tif",id,OIIO::TypeDesc::DOUBLE,std::vector<double>{0,1,4294967295.0,16777217,0,1,4294967295.0,16777217});
    id.set_format(OIIO::TypeDesc::FLOAT);
    write(root/"idsfloat.exr",id,OIIO::TypeDesc::FLOAT,std::vector<float>{0,1,1.5f,nan,0,1,1.5f,nan});
    OIIO::ImageSpec crypto(4,2,3,OIIO::TypeDesc::FLOAT);crypto.channelnames={"Object00.R","Object00.G","Object00.B"};
    crypto.attribute("cryptomatte/1234567/name","Object");
    write(root/"crypto.exr",crypto,OIIO::TypeDesc::FLOAT,std::vector<float>(24,.5f));
    OIIO::ImageSpec cropped(1,1,1,OIIO::TypeDesc::FLOAT);cropped.channelnames={"Z"};
    cropped.x=-2;cropped.y=3;cropped.full_x=-3;cropped.full_y=3;cropped.full_width=4;cropped.full_height=2;
    write(root/"window.exr",cropped,OIIO::TypeDesc::FLOAT,std::vector<float>{5});
    cropped.set_format(OIIO::TypeDesc::UINT32);cropped.channelnames={"object_id"};
    write(root/"idwindow.exr",cropped,OIIO::TypeDesc::UINT32,std::vector<std::uint32_t>{42});
    OIIO::ImageSpec oriented(4,2,1,OIIO::TypeDesc::DOUBLE);oriented.attribute("Orientation",6);
    write(root/"oriented.tif",oriented,OIIO::TypeDesc::DOUBLE,std::vector<double>{0,5,10,15,20,25,30,35});
    OIIO::ImageSpec labels(4,2,3,OIIO::TypeDesc::FLOAT);labels.channelnames={"matID.R","matID.G","matID.B"};
    write(root/"labels.exr",labels,OIIO::TypeDesc::FLOAT,std::vector<float>{1,0,0,0,1,0,0,0,1,.5f,.25f,.75f,1,0,0,0,1,0,0,0,1,.5f,.25f,.75f});
    labels.set_format(OIIO::TypeDesc::UINT8);labels.channelnames={"R","G","B"};
    write(root/"labels.png",labels,OIIO::TypeDesc::UINT8,std::vector<std::uint8_t>{255,0,0,0,255,0,0,0,255,128,64,191,255,0,0,0,255,0,0,0,255,128,64,191});
    Json result;for(const char* name:{"depth.exr","depthgood.exr","depth16.tif","normal.exr","vector.exr","ids.exr","idsdouble.tif","idsfloat.exr","crypto.exr","window.exr","idwindow.exr","oriented.tif","labels.exr","labels.png"})result[name]=(root/name).u8string();
    selection_fixtures(root,result);
    return result;
}
static std::vector<unsigned char> read(const fs::path& path,int width,int height) {
    auto input=OIIO::ImageInput::open(path.u8string());require(bool(input),OIIO::geterror());
    const auto& s=input->spec();require(s.width==width && s.height==height && s.nchannels==3 && s.format==OIIO::TypeDesc::UINT8,"unexpected preview shape/type");
    std::vector<unsigned char> pixels(size_t(width)*height*3);
    require(input->read_image(0,0,0,3,OIIO::TypeDesc::UINT8,pixels.data()),input->geterror());require(input->close(),"close preview");return pixels;
}
static void pixel(const std::vector<unsigned char>& pixels,size_t p,int r,int g,int b) {
    require(pixels[p*3]==r && pixels[p*3+1]==g && pixels[p*3+2]==b,"data display pixel mismatch at "+std::to_string(p));
}
static int run(const std::vector<fs::path>& args) {
    try {
        if(args.size()==3 && args[1]=="--fixtures") {std::cout<<fixtures(args[2]).dump()<<'\n';return 0;}
        auto root=fs::temp_directory_path()/("thumbgen-aov-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto files=fixtures(root);
        struct Cleanup{fs::path root;~Cleanup(){std::error_code ec;if(root.parent_path()==fs::temp_directory_path())fs::remove_all(root,ec);}}cleanup{root};
        thumbgen::Engine engine;
        auto job=[&](const char* source,const char* mode,const char* channels,const std::string& suffix) {
            thumbgen::Job value;value.id=suffix;value.input=files[source];value.options.visualization=mode;value.options.channels=channels;
            value.outputs.push_back({(root/(suffix+".png")).u8string(),"png",4,2,90});return value;
        };
        auto depth=job("depth.exr","depth","Z","depth");depth.options.data_range={0,10};
        auto r=engine.convert(depth);require(r["ok"].get<bool>(),r.dump());
        require(r["visualization"]["invalid_pixels"]==3 && r["visualization"]["clipped_components"]==2,r.dump());
        require(r["colour"]["basis"]=="data_visualization",r.dump());
        auto p=read(root/"depth.png",4,2);pixel(p,0,0,0,0);pixel(p,1,128,128,128);pixel(p,2,255,255,255);pixel(p,3,255,255,255);
        pixel(p,4,0,0,0);for(int i:{5,6,7})pixel(p,i,255,0,255);
        auto d16=job("depth16.tif","depth","GRAY","depth16");d16.options.data_range={0,10};
        r=engine.convert(d16);require(r["ok"].get<bool>(),r.dump());p=read(root/"depth16.png",4,2);pixel(p,1,128,128,128);pixel(p,2,255,255,255);
        auto normal=job("normal.exr","normal","N","normal");r=engine.convert(normal);require(r["ok"].get<bool>(),r.dump());
        p=read(root/"normal.png",4,2);for(int i=0;i<8;++i)pixel(p,i,0,128,255);
        auto vector=job("vector.exr","vector","motion","vector");vector.options.data_range={-2,2};r=engine.convert(vector);require(r["ok"].get<bool>(),r.dump());
        p=read(root/"vector.png",4,2);for(int i=0;i<8;++i)pixel(p,i,0,255,128);
        auto ids=job("ids.exr","id","object_id","ids");r=engine.convert(ids);require(r["ok"].get<bool>() && r["filter"]=="nearest",r.dump());
        p=read(root/"ids.png",4,2);for(int y=0;y<2;++y){pixel(p,y*4,119,178,193);pixel(p,y*4+1,224,142,64);pixel(p,y*4+2,182,146,193);pixel(p,y*4+3,213,140,248);}
        Json planned;engine.set_control({[&](const auto&,const auto& plan){planned=plan;},{},{}});
        auto dbl=job("idsdouble.tif","id","GRAY","idsdouble");r=engine.convert(dbl);require(r["ok"].get<bool>(),r.dump());
        require(planned["footprint"]["source_sample_bytes"]==8,"DOUBLE source bytes missing from budget");
        p=read(root/"idsdouble.png",4,2);pixel(p,2,182,146,193);pixel(p,3,87,192,129);engine.set_control({});
        auto bad=job("idsfloat.exr","id","object_id","idsfloat");r=engine.convert(bad);require(r["ok"].get<bool>() && r["visualization"]["invalid_pixels"]==4,r.dump());
        bad.outputs[0].path=(root/"invalid-error.png").u8string();bad.options.invalid_values="error";r=engine.convert(bad);
        require(!r["ok"].get<bool>() && r["error"]["code"]=="data_invalid_value" && !fs::exists(root/"invalid-error.png"),r.dump());
        auto crypto=job("crypto.exr","id","Object00.R","crypto");r=engine.convert(crypto);
        require(r["error"]["code"]=="cryptomatte_requires_visualization",r.dump());
        normal.options.visualization="depth";normal.options.data_range={0,10};r=engine.convert(normal);require(r["error"]["code"]=="visualization_layout_unsupported",r.dump());
        depth.sequence_id="depth-sequence";depth.options.invalid_values="error";depth.outputs[0].path=(root/"sequence-bad.png").u8string();r=engine.convert(depth);
        require(r["error"]["code"]=="data_invalid_value",r.dump());auto pin=engine.sequence_lock(depth.sequence_id);require(!pin.is_null(),"data selection not locked before mapping");
        thumbgen::Engine replacement;replacement.restore_sequence_lock(depth.sequence_id,pin);
        depth.input=files["depthgood.exr"];depth.outputs[0].path=(root/"sequence-good.png").u8string();r=replacement.convert(depth);
        require(r["ok"].get<bool>() && r["selection"]["lock_status"]=="reused",r.dump());
        depth.options.data_range={0,20};r=replacement.convert(depth);require(r["error"]["code"]=="sequence_selection_conflict",r.dump());
        auto window=job("window.exr","depth","Z","window");window.options.data_range={0,10};r=engine.convert(window);require(r["ok"].get<bool>(),r.dump());
        p=read(root/"window.png",4,2);for(int i=0;i<8;++i)pixel(p,i,i==1?128:0,i==1?128:0,i==1?128:0);
        auto idwindow=job("idwindow.exr","id","object_id","idwindow");r=engine.convert(idwindow);require(r["ok"].get<bool>(),r.dump());
        p=read(root/"idwindow.png",4,2);for(int i=0;i<8;++i)pixel(p,i,i==1?213:0,i==1?140:0,i==1?248:0);
        auto oriented=job("oriented.tif","depth","GRAY","oriented");oriented.options.data_range={0,35};oriented.outputs[0].height=4;
        r=engine.convert(oriented);require(r["ok"].get<bool>(),r.dump());p=read(root/"oriented.png",2,4);
        const int rotated[]={146,0,182,36,219,73,255,109};for(int i=0;i<8;++i)pixel(p,i,rotated[i],rotated[i],rotated[i]);
        auto labels=job("labels.exr","id_rgb","matID","labels-preview");labels.options.data_range={0,1};labels.outputs[0].width=2;labels.outputs[0].height=1;
        r=engine.convert(labels);require(r["ok"].get<bool>() && r["filter"]=="nearest",r.dump());p=read(root/"labels-preview.png",2,1);pixel(p,0,0,255,0);pixel(p,1,128,64,191);
        labels.input=files["labels.png"];labels.options.channels="RGB";labels.options.data_range={0,255};labels.outputs[0].path=(root/"labels8.png").u8string();
        r=engine.convert(labels);require(r["ok"].get<bool>(),r.dump());require(read(root/"labels8.png",2,1)==p,"encoded integer RGB labels normalized as colour");
        thumbgen::Engine changing([&](const std::string& phase,const auto&){if(phase=="decoded"){std::ofstream stream(root/"window.exr",std::ios::binary|std::ios::app);stream.put('x');}});
        window.outputs[0].path=(root/"source-changed.png").u8string();r=changing.convert(window);
        require(r["error"]["code"]=="source_changed" && !fs::exists(root/"source-changed.png"),r.dump());
        std::cout<<"native AOV display mapping, categorical resize, type-aware budget and sequence restoration passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int count,wchar_t** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#else
int main(int count,char** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#endif
