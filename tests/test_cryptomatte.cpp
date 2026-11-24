#include "cryptomatte.h"
#include "files.h"
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
using namespace thumbgen::detail;
static void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
// Observe the public reader calls while delegating decoding to a real EXR.
// A failed strip must propagate directly; never retry with the whole reader.
class ReadWitness final : public OIIO::ImageInput {
    OIIO::ImageInput& source_;
    const int fail_;
    const char* decoder_;
public:
    int strip_calls=0,whole_calls=0;
    ReadWitness(OIIO::ImageInput& source,int fail=0,const char* decoder="openexr"):
        source_(source),fail_(fail),decoder_(decoder){}
    const char* format_name() const override{return decoder_;}
    bool open(const std::string&,OIIO::ImageSpec&) override{return false;}
    bool close() override{return true;}
    bool read_native_scanline(int,int,int,int,void*) override{return false;}
    bool read_scanlines(int part,int mip,int begin,int end,int z,int first,int last,
        OIIO::TypeDesc type,void* data,OIIO::stride_t xs,OIIO::stride_t ys) override {
        if(++strip_calls==fail_)return false;
        return source_.read_scanlines(part,mip,begin,end,z,first,last,type,data,xs,ys);
    }
    bool read_image(int part,int mip,int first,int last,OIIO::TypeDesc type,void* data,
        OIIO::stride_t xs,OIIO::stride_t ys,OIIO::stride_t zs,OIIO::ProgressCallback callback,void* context) override {
        ++whole_calls;return source_.read_image(part,mip,first,last,type,data,xs,ys,zs,callback,context);
    }
};
static constexpr std::uint32_t bunny=0x13851a76U,other=0x42c9679fU;
static void metadata(OIIO::ImageSpec& spec,const std::string& prefix="cryptomatte/test123") {
    spec.attribute(prefix+"/name","Object");spec.attribute(prefix+"/hash","MurmurHash3_32");spec.attribute(prefix+"/conversion","uint32_to_float32");
    spec.attribute("DateTime","2000:01:01 00:00:00");
}
static void object_family_fixtures(const fs::path& root,Json& files) {
    auto save=[&](const char* fixture,const char* object,const char* secondary) {
        const std::string family=object,other_family=secondary,kind=fixture;
        const auto path=(root/(kind+".exr")).u8string();
        OIIO::ImageSpec parts[2];
        for(int part=0;part<2;++part) {
            std::vector<std::string> channels;
            for(char c:std::string("RGBA"))channels.push_back(family+(part?"01.":"00.")+c);
            if(part)for(char c:std::string("RGBA"))channels.push_back(other_family+"00."+c);
            if(part && kind=="beauty_and_families")channels.insert(channels.end(),{"R","G","B"});
            if(part && kind=="object_bad_layout")channels.erase(channels.begin()+3);
            parts[part]=OIIO::ImageSpec(2,2,int(channels.size()),OIIO::TypeDesc::FLOAT);
            parts[part].channelnames=std::move(channels);
            parts[part].attribute("oiio:subimagename",part?"second":"first");
        }
        metadata(parts[0]);
        parts[0].attribute("cryptomatte/test123/name",family);
        if(kind=="object_bad_metadata")parts[0].attribute("cryptomatte/test123/hash","wrong_hash");
        metadata(parts[1],"cryptomatte/test456");
        parts[1].attribute("cryptomatte/test456/name",other_family);
        auto out=OIIO::ImageOutput::create(path);require(bool(out),OIIO::geterror());
        require(out->open(path,2,parts),out->geterror());
        for(int part=0;part<2;++part) {
            if(part)require(out->open(path,parts[part],OIIO::ImageOutput::AppendSubimage),out->geterror());
            std::vector<float> pixels(4*parts[part].nchannels);
            for(std::size_t c=0;c<pixels.size();++c) {
                const auto& channel=parts[part].channelnames[c%parts[part].nchannels];
                if(channel.find('.')==std::string::npos)pixels[c]=.5f;
                else if(channel.back()=='R') {
                    const auto id=channel.rfind(other_family,0)==0?other:bunny;
                    std::memcpy(&pixels[c],&id,sizeof(id));
                } else if(channel.back()=='G')pixels[c]=channel.rfind(other_family,0)==0?1.0f:.5f;
            }
            require(out->write_image(OIIO::TypeDesc::FLOAT,pixels.data()),out->geterror());
        }
        require(out->close(),out->geterror());
        files[kind]=path;
    };
    save("object_underscore","CRYPTO_OBJECT","crypto_material");
    save("object_camel","CryptoObject","CryptoMaterial");
    save("mislabeled_object","Crypto_Object_Extra","CryptoMaterial");
    save("two_objects","CryptoObject","crypto_object");
    save("object_bad_metadata","CryptoObject","CryptoMaterial");
    save("object_bad_layout","CryptoObject","CryptoMaterial");
    save("beauty_and_families","CryptoObject","CryptoMaterial");
}
static Json fixtures(const fs::path& root) {
    require(fs::create_directory(root),"fixture directory already exists");Json result;
    for(const std::string kind:{"valid","with_beauty","multiple_families","bad","half_coverage","half_id","odd","gap","no_metadata","bad_hash","sidecar","legacy","many","window","oriented","missing_last","identity","strip_float","strip_half","strip_bad","strip_window","strip_tiled","strip_interleaved","strip_zips"}) {
        int width=kind.rfind("strip_",0)==0?128:kind=="many"?128:kind=="identity"?1280:kind=="window"?2:kind=="oriented"?2:8;
        int height=kind.rfind("strip_",0)==0?257:kind=="many"?128:kind=="window"?2:kind=="oriented" || kind=="identity"?3:1;
        int layers=kind=="many"?10:3;
        std::vector<std::string> names=kind=="with_beauty"?std::vector<std::string>{"R","G","B"}:
            std::vector<std::string>{"Object.R","Object.G","Object.B"};
        std::vector<int> roles(3,-1);
        for(int layer=0;layer<layers;++layer) {
            auto name="Object"+(layer==1 && kind=="gap"?std::string("03"):layer<10?"0"+std::to_string(layer):std::to_string(layer));
            for(int c=0;c<((kind=="odd" || kind=="missing_last") && layer==layers-1?kind=="odd"?2:3:4);++c) {
                names.push_back(name+"."+"RGBA"[c]);roles.push_back(layer*4+c);
            }
        }
        if(kind=="strip_interleaved") {
            names.insert(names.begin()+7,"Object00.unrelated");roles.insert(roles.begin()+7,-1);
        }
        if(kind=="multiple_families")for(int c=0;c<4;++c) {
            names.push_back(std::string("Asset00.")+"RGBA"[c]);roles.push_back(c);
        }
        OIIO::ImageSpec spec(width,height,int(names.size()),OIIO::TypeDesc::FLOAT);spec.channelnames=names;
        if(kind=="strip_tiled"){spec.tile_width=32;spec.tile_height=32;}
        spec.attribute("compression",kind=="strip_zips"?"zips":"zip");
        metadata(spec,kind=="legacy"?"exr/cryptomatte/test123":"cryptomatte/test123");
        if(kind=="multiple_families") {
            metadata(spec,"cryptomatte/test456");
            spec.attribute("cryptomatte/test456/name","Asset");
        }
        if(kind=="no_metadata")for(const char* field:{"name","hash","conversion"})spec.erase_attribute(std::string("cryptomatte/test123/")+field);
        if(kind=="bad_hash")spec.attribute("cryptomatte/test123/hash","wrong_hash");
        if(kind=="sidecar")spec.attribute("cryptomatte/test123/manif_file","missing.json");
        else spec.attribute("cryptomatte/test123/manifest","this optional manifest deliberately is not JSON");
        if(kind=="window" || kind=="strip_window"){spec.x=-1;spec.y=5;spec.full_x=-3;spec.full_y=4;spec.full_width=6;spec.full_height=4;}
        if(kind=="oriented")spec.attribute("Orientation",6);
        spec.channelformats.resize(names.size(),OIIO::TypeDesc::FLOAT);
        for(std::size_t c=3;c<roles.size();++c)if(((kind=="half_coverage" || kind=="strip_half") && roles[c]%2==1) || (kind=="half_id" && roles[c]%2==0))spec.channelformats[c]=OIIO::TypeDesc::HALF;
        std::vector<unsigned char> data(std::size_t(width)*height*spec.pixel_bytes(true));
        for(int p=0;p<width*height;++p) {
            std::uint32_t ids[6]={};float weights[6]={};
            if(kind=="many" || kind=="window"){ids[0]=bunny;weights[0]=1;}
            else if(kind=="oriented"){ids[0]=p%2?other:bunny;weights[0]=1;}
            else switch(p%8) {
                case 0:ids[0]=bunny;weights[0]=1;break;
                case 1:ids[0]=bunny;weights[0]=0.25f;ids[1]=other;weights[1]=0.75f;break;
                case 2:ids[0]=ids[2]=bunny;weights[0]=0.25f;weights[2]=0.75f;break;
                case 3:weights[0]=1;break;
                case 4:ids[0]=0xf19e1d35U;weights[0]=0.5f;break;
                case 5:ids[0]=0x3f800001U;ids[1]=0x3f800002U;weights[0]=weights[1]=0.5f;break;
                case 6:ids[0]=bunny;ids[1]=other;weights[0]=1.25f;weights[1]=-0.25f;break;
                default:ids[0]=0x7fc00001U;weights[0]=0;ids[1]=other;weights[1]=1;break;
            }
            if((kind=="bad" || kind=="strip_bad") && p%128==1){ids[0]=0x7fc00001U;weights[0]=0.25f;}
            if((kind=="bad" || kind=="strip_bad") && p%128==2){weights[2]=std::numeric_limits<float>::infinity();}
            auto* target=data.data()+std::size_t(p)*spec.pixel_bytes(true);
            for(std::size_t c=0;c<names.size();++c) {
                int role=roles[c];std::uint32_t bits=0;float weight=0;
                if(role>=0 && role/2<6){bits=ids[role/2];weight=weights[role/2];}
                if(spec.channelformat(int(c))==OIIO::TypeDesc::HALF){half h=role%2==1?half(weight):half(0);std::memcpy(target,&h,2);target+=2;}
                else {
                    if(role<0 && kind=="with_beauty")weight=0.125f+0.125f*float(c);
                    if((role>=0 && role%2==1) || (role<0 && kind=="with_beauty"))std::memcpy(target,&weight,4);
                    else std::memcpy(target,&bits,4);
                    target+=4;
                }
            }
        }
        auto path=root/(kind+".exr");auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output),OIIO::geterror());
        require(output->open(path.u8string(),spec),output->geterror());require(output->write_image(OIIO::TypeDesc::UNKNOWN,data.data()),output->geterror());require(output->close(),output->geterror());
        result[kind]=path.u8string();
    }
    auto path=root/"split.exr";OIIO::ImageSpec parts[2]={OIIO::ImageSpec(2,2,4,OIIO::TypeDesc::FLOAT),OIIO::ImageSpec(2,2,4,OIIO::TypeDesc::FLOAT)};
    metadata(parts[0]);
    for(int i=0;i<2;++i){parts[i].attribute("oiio:subimagename",i?"right":"left");for(int c=0;c<4;++c)parts[i].channelnames[c]="Object0"+std::to_string(i)+"."+"RGBA"[c];}
    auto output=OIIO::ImageOutput::create(path.u8string());require(bool(output) && output->open(path.u8string(),2,parts),"open split fixture");float pixels[16]={};
    std::memcpy(pixels,&bunny,sizeof(bunny));pixels[1]=0.25f;
    require(output->write_image(OIIO::TypeDesc::FLOAT,pixels),output->geterror());require(output->open(path.u8string(),parts[1],OIIO::ImageOutput::AppendSubimage),output->geterror());
    std::memcpy(pixels,&other,sizeof(other));pixels[1]=0.75f;
    require(output->write_image(OIIO::TypeDesc::FLOAT,pixels),output->geterror());require(output->close(),output->geterror());result["split"]=path.u8string();
    object_family_fixtures(root,result);
    return result;
}
static int run(const std::vector<fs::path>& args) {
    try {
        if(args.size()==3 && args[1]=="--fixtures"){std::cout<<fixtures(args[2]).dump()<<'\n';return 0;}
        auto root=fs::temp_directory_path()/("thumbgen-crypto-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));auto files=fixtures(root);
        struct Cleanup{fs::path root;~Cleanup(){std::error_code ec;if(root.parent_path()==fs::temp_directory_path())fs::remove_all(root,ec);}}cleanup{root};
        auto input=OIIO::ImageInput::open(files["valid"].get<std::string>());require(bool(input),OIIO::geterror());
        auto layout=crypto_layout({input->spec()},"Object");require(layout.layers.size()==3 && layout.channel_names.size()==12,"metadata/rank discovery");
        auto image=read_crypto_image(*input,layout,InvalidData::error,1024*1024,3);require(input->close(),"close input");
        require(image.statistics.invalid_pixels==0,"valid signed/overshoot/unused-ID values rejected");
        float p0[4],p2[4],background[4];image.image.getpixel(0,0,p0,4);image.image.getpixel(2,0,p2,4);image.image.getpixel(3,0,background,4);
        require(std::memcmp(p0,p2,sizeof(p0))==0 && p0[3]==1 && background[0]==0 && background[3]==0,"rank accumulation/background semantics");
        // Compare bounded all-rank strips against the retained whole-layer path,
        // including partial last strips, native mixed types and nonzero windows.
        for(const char* kind:{"strip_float","strip_half","strip_bad","strip_window","strip_tiled","strip_interleaved","strip_zips"}) {
            auto source=OIIO::ImageInput::open(files[kind].get<std::string>());
            auto spec=source->spec();auto ranks=crypto_layout({spec},"Object");
            ReadWitness witness(*source);
            auto strip=read_crypto_image(witness,ranks,InvalidData::magenta,8*1024*1024,1);
            const bool fallback=std::string(kind)=="strip_tiled" || std::string(kind)=="strip_interleaved";
            require(fallback?(witness.strip_calls==0 && witness.whole_calls>0):
                (witness.strip_calls==5 && witness.whole_calls==0),"strip/fallback qualification changed");
            require(source->close(),"close strip input");
            source=OIIO::ImageInput::open(files[kind].get<std::string>());
            spec.tile_width=1; // Select the original whole-layer fallback without changing file contents.
            auto whole=read_crypto_image(*source,crypto_layout({spec},"Object"),InvalidData::magenta,8*1024*1024,1);
            require(source->close(),"close fallback input");
            require(strip.decoded_bytes==whole.decoded_bytes &&
                strip.statistics.invalid_pixels==whole.statistics.invalid_pixels &&
                strip.statistics.clipped_components==whole.statistics.clipped_components,"strip statistics differ");
            require(std::memcmp(strip.image.localpixels(),whole.image.localpixels(),std::size_t(spec.width)*spec.height*4*sizeof(float))==0,"strip rank order/native bits differ");
        }
        thumbgen::Engine engine;auto job=[&](const char* name,const char* suffix){thumbgen::Job j;j.id=suffix;j.input=files[name];j.options.visualization="cryptomatte";j.options.channels="Object";j.outputs.push_back({(root/(std::string(suffix)+".png")).u8string(),"png",128,128,90});return j;};
        auto first=job("bad","bad-error");first.sequence_id="crypto-sequence";first.options.invalid_values="error";auto result=engine.convert(first);
        require(result["error"]["code"]=="data_invalid_value" && result["visualization"]["invalid_pixels"]==2 && !fs::exists(first.outputs[0].path),result.dump());
        auto lock=engine.sequence_lock(first.sequence_id);thumbgen::Engine replacement;replacement.restore_sequence_lock(first.sequence_id,lock);
        auto second=job("valid","restored");second.sequence_id=first.sequence_id;second.options.invalid_values="error";result=replacement.convert(second);
        require(result["ok"].get<bool>() && result["selection"]["lock_status"]=="reused",result.dump());
        auto many=job("many","bounded");many.options.max_decode_mib=1;result=engine.convert(many);require(result["ok"].get<bool>() && result["decoded_mib"]==0.25 && result["cryptomatte"]["ranks"]==20,result.dump());
        auto split=job("split","incomplete");split.options.part_name="left";result=engine.convert(split);require(result["error"]["code"]=="cryptomatte_layout_unsupported" && !fs::exists(split.outputs[0].path),result.dump());
        auto split_source=OIIO::ImageInput::open(files["split"].get<std::string>());
        require(bool(split_source),"open split rank fixture");
        auto left=split_source->spec();OIIO::ImageSpec right;
        require(split_source->seek_subimage(1,0,right) && split_source->close(),"read split rank metadata");
        auto reject_split=[&](const OIIO::ImageSpec& other_part,const char* code,const char* message) {
            try{crypto_layout({left,other_part},"Object");}
            catch(const FileFailure& error){require(error.code==code,error.what());return;}
            throw std::runtime_error(message);
        };
        auto offset=right;++offset.x;reject_split(offset,"cryptomatte_layout_unsupported","misaligned Cryptomatte parts accepted");
        auto duplicate=right;duplicate.channelnames=left.channelnames;reject_split(duplicate,"ambiguous_parts","duplicate Cryptomatte layers accepted");
        auto selected_copy=crypto_layout({left,duplicate},"Object",1);
        require(selected_copy.parts.size()==1 && selected_copy.parts.count(1)==1,"explicit selection of a complete duplicate family failed");
        OIIO::ImageSpec beauty(2,2,3,OIIO::TypeDesc::FLOAT);
        beauty.attribute("cryptomatte/test123/name","Object");
        auto with_beauty=crypto_layout({beauty,left,right},"Object");
        require(with_beauty.parts.size()==2 && with_beauty.layers.size()==2,"rankless part metadata overrode rank metadata");
        auto truncated=root/"strip-truncated.exr";
        fs::copy_file(files["strip_float"].get<std::string>(),truncated);
        fs::resize_file(truncated,fs::file_size(truncated)-64);
        auto broken=OIIO::ImageInput::open(truncated.u8string());require(bool(broken),"open truncated strip header");
        auto broken_layout=crypto_layout({broken->spec()},"Object");
        bool refused=false;
        try{read_crypto_image(*broken,broken_layout,InvalidData::magenta,8*1024*1024,1);}
        catch(const FileFailure& failure){require(failure.code=="decode_failed","strip failure classification changed");refused=true;}
        require(refused,"partial strip failure accepted");broken->close();
        for(int fail:{1,3,5}) {
            auto real=OIIO::ImageInput::open(files["strip_zips"].get<std::string>());
            auto ranks=crypto_layout({real->spec()},"Object");ReadWitness failing(*real,fail);
            bool rejected=false;
            try{read_crypto_image(failing,ranks,InvalidData::magenta,8*1024*1024,1);}
            catch(const FileFailure& error){require(error.code=="decode_failed","injected strip error changed");rejected=true;}
            require(rejected && failing.strip_calls==fail && failing.whole_calls==0,"failed strip was reread");
            require(real->close(),"close injected strip input");
        }
        auto real=OIIO::ImageInput::open(files["strip_float"].get<std::string>());
        auto foreign_ranks=crypto_layout({real->spec()},"Object");
        ReadWitness foreign(*real,0,"unmeasured-codec");
        read_crypto_image(foreign,foreign_ranks,InvalidData::magenta,8*1024*1024,1);
        require(foreign.strip_calls==0 && foreign.whole_calls==3,"unmeasured decoder entered EXR strips");real->close();
        auto failed_job=job("strip_float","strip-error-stage");failed_job.input=truncated.u8string();failed_job.options.threads=1;
        auto failed_result=engine.convert(failed_job);
        require(failed_result["error"]["code"]=="decode_failed" && failed_result["error"]["stage"]=="visualization" &&
            !fs::exists(failed_job.outputs[0].path),"strip error stage/output changed");
        auto recovery=job("strip_half","strip-recovery");recovery.options.threads=1;
        require(engine.convert(recovery)["ok"].get<bool>(),"task after strip failure did not recover");
        auto plain=job("valid","default");plain.options.visualization="colour";plain.options.channels="";result=engine.convert(plain);require(result["error"]["code"]=="visualization_required",result.dump());
        std::cout<<"Cryptomatte opaque IDs, streamed ranks, coverage overview, metadata and stable sequence passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int count,wchar_t** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#else
int main(int count,char** values){std::vector<fs::path> args;for(int i=0;i<count;++i)args.emplace_back(values[i]);return run(args);}
#endif
