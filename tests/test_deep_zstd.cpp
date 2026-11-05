#include "deep_reader.h"
#include "deep_flatten.h"
#include "files.h"
#include "deep_reference.h"
#include "deep_background_reference.h"
#include <OpenEXR/openexr.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfDeepScanLineOutputFile.h>
#include <OpenEXR/ImfDeepTiledOutputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <Imath/half.h>
#include <nlohmann/json.hpp>
#include <zstd.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <vector>
using namespace thumbgen::detail;
namespace fs=std::filesystem;
static void need(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
static DeepChannels mapping(){return {{"R","G","B"},{"AR","AG","AB"},"Z","Zback"};}
static std::string bytes(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void save(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());need(bool(f),"write failed");}
static std::vector<DeepSample> points(){return {{2,2,{0,.75f,.5f},{0,.75f,.5f}},{1,1,{.5f,.25f,0},{.5f,.25f,0}}};}
static void fixture(const fs::path& p,const std::vector<DeepSample>& samples,bool tile=false,bool mixed=false,bool vary=false,int rows=1,int width=128){
    namespace Imf=OPENEXR_IMF_NAMESPACE;
    Imf::Header header(width,rows);header.setType(tile?Imf::DEEPTILE:Imf::DEEPSCANLINE);header.compression()=Imf::ZSTD_COMPRESSION;
    if(tile)header.setTileDescription(Imf::TileDescription(64,2,Imf::ONE_LEVEL));
    const char* names[]={"R","G","B","AR","AG","AB","Z","Zback","unused_id"};
    std::vector<unsigned int> counts(width*rows);std::size_t total=0;
    for(int y=0;y<rows;++y)for(int x=0;x<width;++x){auto n=vary?(y%4==1?1:y%4==2?int(samples.size()):0):int(samples.size());counts[y*width+x]=n;total+=n;}
    Imf::DeepFrameBuffer frame;frame.insertSampleCountSlice(Imf::Slice(Imf::UINT,reinterpret_cast<char*>(counts.data()),4,width*4));
    std::array<std::vector<float>,9> floats;std::array<std::vector<IMATH_NAMESPACE::half>,9> halves;std::vector<unsigned int> ids;
    std::array<std::vector<char*>,9> pointers;
    for(int c=0;c<9;++c){auto type=mixed?(c<6?Imf::HALF:c==8?Imf::UINT:Imf::FLOAT):Imf::FLOAT;header.channels().insert(names[c],Imf::Channel(type));
        if(type==Imf::HALF)halves[c].resize(total);else if(type==Imf::UINT)ids.resize(total);else floats[c].resize(total);
        pointers[c].resize(counts.size());std::size_t offset=0;
        for(std::size_t pixel=0;pixel<counts.size();++pixel){
            pointers[c][pixel]=type==Imf::HALF?reinterpret_cast<char*>(halves[c].data()+offset):type==Imf::UINT?reinterpret_cast<char*>(ids.data()+offset):reinterpret_cast<char*>(floats[c].data()+offset);
            for(unsigned int n=0;n<counts[pixel];++n){const auto& sample=samples[n];float v=c<3?sample.colour[c]:c<6?sample.alpha[c-3]:c==6?sample.z:c==7?sample.zback:17;
                if(type==Imf::HALF)halves[c][offset+n]=v;else if(type==Imf::UINT)ids[offset+n]=17;else floats[c][offset+n]=v;}
            offset+=counts[pixel];
        }
        frame.insert(names[c],Imf::DeepSlice(type,reinterpret_cast<char*>(pointers[c].data()),sizeof(char*),width*sizeof(char*),type==Imf::HALF?2:4));
    }
    if(tile){Imf::DeepTiledOutputFile out(p.u8string().c_str(),header,0);out.setFrameBuffer(frame);out.writeTiles(0,out.numXTiles()-1,0,out.numYTiles()-1);}
    else{Imf::DeepScanLineOutputFile out(p.u8string().c_str(),header,0);out.setFrameBuffer(frame);out.writePixels(rows);}

}
struct Chunks {std::vector<exr_chunk_info_t> values;int compressed_counts=0,compressed_samples=0,raw_counts=0,raw_samples=0;};
static Chunks inspect(const fs::path& p,bool tiled=false){
    exr_context_t ctx=nullptr;need(exr_start_read(&ctx,p.u8string().c_str(),nullptr)==EXR_ERR_SUCCESS,"inspect open");
    struct Finish{exr_context_t& c;~Finish(){exr_finish(&c);}} finish{ctx};
    exr_compression_t comp;need(exr_get_compression(ctx,0,&comp)==EXR_ERR_SUCCESS && int(comp)==13,"actual native compression must be 13");
    Chunks result;exr_attr_box2i_t w;need(exr_get_data_window(ctx,0,&w)==0,"window");
    if(tiled){int32_t nx,ny;need(exr_get_tile_counts(ctx,0,0,0,&nx,&ny)==0,"tiles");for(int y=0;y<ny;++y)for(int x=0;x<nx;++x){exr_chunk_info_t c{};need(exr_read_tile_chunk_info(ctx,0,x,y,0,0,&c)==0,"tile info");result.values.push_back(c);}}
    else for(int y=w.min.y;y<=w.max.y;){exr_chunk_info_t c{};need(exr_read_scanline_chunk_info(ctx,0,y,&c)==0,"scan info");result.values.push_back(c);y+=c.height;}
    const auto data=bytes(p);const char magic[8]={'z','s','t','d','-','e','x','r'};
    auto compressed=[&](uint64_t off,uint64_t size){need(off<=data.size() && size<=data.size()-off,"payload range");return size>=24 && std::memcmp(data.data()+off,magic,8)==0;};
    for(const auto& c:result.values){if(compressed(c.sample_count_data_offset,c.sample_count_table_size))++result.compressed_counts;else ++result.raw_counts;
        if(c.packed_size){if(compressed(c.data_offset,c.packed_size))++result.compressed_samples;else ++result.raw_samples;}}
    return result;
}
// Independent, scalar wire constructor: no OpenEXR codec or product decoding.
// Restricted to this test's single-row all-FLOAT fixture, with known samples.
static void put64(std::string& s,std::size_t offset,std::uint64_t v){for(int b=0;b<8;++b)s[offset+b]=char(v>>(b*8));}
static std::string independent_frame(std::vector<std::vector<std::uint32_t>> channels,bool delta,bool unknown=false){
    std::vector<std::uint32_t> words;
    for(auto channel:channels){if(delta)for(std::size_t i=channel.size();i>1;--i)channel[i-1]-=channel[i-2];words.insert(words.end(),channel.begin(),channel.end());}
    std::string inner(8+words.size()*4,'\0');put64(inner,0,words.size()*4);
    for(std::size_t i=0;i<words.size();++i)for(int b=0;b<4;++b)inner[8+b*words.size()+i]=char(words[i]>>(8*b));
    std::string payload(ZSTD_compressBound(inner.size()),'\0');size_t size;
    if(unknown){auto cctx=ZSTD_createCCtx();need(cctx!=nullptr,"test compressor");ZSTD_CCtx_setParameter(cctx,ZSTD_c_contentSizeFlag,0);size=ZSTD_compress2(cctx,payload.data(),payload.size(),inner.data(),inner.size());ZSTD_freeCCtx(cctx);}
    else size=ZSTD_compress(payload.data(),payload.size(),inner.data(),inner.size(),3);need(!ZSTD_isError(size),"independent ZSTD compression");payload.resize(size);
    std::string frame(24,'\0');std::memcpy(frame.data(),"zstd-exr",8);frame[8]=delta?2:1;frame[12]=delta?1:0;put64(frame,16,payload.size());return frame+payload;
}
static void independent_wire(const fs::path& p,bool delta,bool raw_counts=false,bool unknown=false){
    auto info=inspect(p);need(info.values.size()==1,"independent one chunk");auto chunk=info.values[0];auto data=bytes(p);auto original=data;
    const auto samples=points();std::vector<std::uint32_t> counts(128);for(unsigned int x=0;x<128;++x)counts[x]=(x+1)*2;
    std::string count_frame;
    if(raw_counts){count_frame.resize(512);for(std::size_t i=0;i<counts.size();++i)for(int b=0;b<4;++b)count_frame[i*4+b]=char(counts[i]>>(8*b));}
    else count_frame=independent_frame({counts},delta,unknown);
    // Header-sorted AB, AG, AR, B, G, R, Z, Zback, unused_id.
    const int slots[]={5,4,3,2,1,0,6,7,8};std::vector<std::vector<std::uint32_t>> channels;
    for(auto c:slots){std::vector<std::uint32_t> words;for(int x=0;x<128;++x)for(auto sample:samples){float v=c<3?sample.colour[c]:c<6?sample.alpha[c-3]:c==6?sample.z:c==7?sample.zback:17;std::uint32_t bits;std::memcpy(&bits,&v,4);words.push_back(bits);}channels.push_back(words);}
    auto sample_frame=independent_frame(channels,delta,unknown);data.resize(chunk.sample_count_data_offset);put64(data,chunk.sample_count_data_offset-24,count_frame.size());put64(data,chunk.sample_count_data_offset-16,sample_frame.size());data+=count_frame;data+=sample_frame;save(p,data);
}
template<class F>static void reject(const char* code,F f){try{f();}catch(const DeepFailure& e){need(e.code==std::string(code),std::string("wrong error ")+e.code+": "+e.what());return;}throw std::runtime_error("unexpected acceptance");}
static void check_pixels(const DeepImage& image,bool vary=false){const float pair[]={.5f,.8125f,.5f,.5f,.8125f,.5f},single[]={0,.75f,.5f,0,.75f,.5f};
    for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x)for(int c=0;c<6;++c){float expected=vary?(y%4==1?single[c]:y%4==2?pair[c]:0):pair[c];need(image.pixels[(y*image.width+x)*6+c]==expected,"mixed/empty decoded value");}}
int main(int argc,char** argv){try{
    const std::string mode=argc>1?argv[1]:"success";
    auto root=fs::temp_directory_path()/("thumbgen-zstd-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));need(fs::create_directory(root),"temp dir");
    struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove_all(p,ec);}} cleanup{root};auto p=root/"input.exr";
    if(mode=="fixture"){need(argc==4,"fixture path/tile");fixture(fs::u8path(argv[2]),points(),std::string(argv[3])=="tile",true,true,9,129);inspect(fs::u8path(argv[2]),std::string(argv[3])=="tile");return 0;}
    fixture(p,points());auto wire=inspect(p);need(wire.compressed_counts>0 && wire.compressed_samples>0,"count AND sample payload must actually be compressed");
    if(!bounded_deep_zstd_available()){reject("deep_layout_unsupported",[&]{read_deep_image(p,0,mapping());});std::cout<<"{\"bounded\":false,\"legacy_rejected\":true}\n";return 0;}
    if(mode=="fault"){
        need(argc==3,"fault index");fixture(p,points(),true,true,true,9,129);DeepReadLimits limits;limits.fail_core_allocation=std::stoull(argv[2]);
        reject("deep_decode_limit_exceeded",[&]{read_deep_image(p,0,mapping(),limits);});check_pixels(read_deep_image(p,0,mapping()),true);
        std::cout<<"{\"fault\":"<<limits.fail_core_allocation<<",\"recovered\":true}\n";return 0;
    }
    if(mode=="corrupt"){
        need(argc==3,"corruption mode");const auto c=wire.values.at(0);auto data=bytes(p);const auto original=data;const auto kind=std::string(argv[2]);
        if(kind=="fds-count" || kind=="fds-sample"){
            const auto off=(kind=="fds-count"?c.sample_count_data_offset:c.data_offset)+24;
            // Valid ZSTD single-segment header with an 8-byte FDS = UINT64_MAX-1.
            const unsigned char header[]={0x28,0xb5,0x2f,0xfd,0xe0,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
            need(off+sizeof(header)<=data.size(),"FDS mutation range");std::memcpy(&data[off],header,sizeof(header));
        }else if(kind=="fds-zero"){const unsigned char empty[]={0x28,0xb5,0x2f,0xfd,0x20,0,1,0,0};std::memcpy(&data[c.data_offset+24],empty,sizeof(empty));}
        else if(kind=="wire-version")data[c.data_offset+8]=3;
        else if(kind=="wire-flags")data[c.data_offset+12]=2;
        else if(kind=="payload-size")std::memset(&data[c.data_offset+16],0xff,8);
        else if(kind=="truncated-frame")data[c.data_offset+24]=0;
        else if(kind=="declared-size"){const char size[8]={0,0,0,1,0,0,0,0};std::memcpy(&data[c.sample_count_data_offset-8],size,8);}
        else throw std::runtime_error("unknown corrupt mode");
        save(p,data);DeepReadLimits corrupt_limits;corrupt_limits.bytes=8*1024*1024;reject(kind=="declared-size"?"deep_decode_limit_exceeded":"deep_decode_failed",[&]{read_deep_image(p,0,mapping(),corrupt_limits);});save(p,original);check_pixels(read_deep_image(p,0,mapping()));
        std::cout<<"{\"corrupt\":\""<<kind<<"\",\"recovered\":true}\n";return 0;
    }
    int layouts=0;
    std::size_t requests=0;
    for(bool tiled:{false,true})for(bool mixed:{false,true}){
        fixture(p,points(),tiled,mixed,true,9,129);auto chunks=inspect(p,tiled);need(chunks.compressed_counts>0 && chunks.compressed_samples>0,"layout compressed path missing");
        auto original=bytes(p);auto image=read_deep_image(p,0,mapping());check_pixels(image,true);requests=std::max(requests,image.core_allocation_requests);
        DeepReadLimits limits;limits.bytes=image.peak_accounted_bytes;check_pixels(read_deep_image(p,0,mapping(),limits),true);--limits.bytes;reject("deep_decode_limit_exceeded",[&]{read_deep_image(p,0,mapping(),limits);});
        int counts=0;try{read_deep_image(p,0,mapping(),{},[&](const char* phase){if(std::string(phase)=="counts" && ++counts==3)throw std::runtime_error("cancel");});throw std::runtime_error("cancel ignored");}catch(const std::runtime_error& e){need(std::string(e.what())=="cancel","cancel classification");}
        need(bytes(p)==original,"input modified");check_pixels(read_deep_image(p,0,mapping()),true);++layouts;
    }
    fixture(p,points());
    for(int limit_kind=0;limit_kind<3;++limit_kind){DeepReadLimits limited;
        if(limit_kind==0)limited.samples_per_pixel=1;
        else if(limit_kind==1)limited.samples_per_chunk=1;
        else limited.samples_per_image=1;
        reject("deep_sample_limit_exceeded",[&]{read_deep_image(p,0,mapping(),limited);});
    }
    // Exact fault-loop fixture; allocation sites include metadata/state/DCtx/shuffle/growth.
    fixture(p,points(),true,true,true,9,129);requests=read_deep_image(p,0,mapping()).core_allocation_requests;
    auto baseline=read_deep_image(p,0,mapping());for(int i=0;i<8;++i){auto again=read_deep_image(p,0,mapping());need(again.core_allocation_requests==baseline.core_allocation_requests && again.peak_accounted_bytes==baseline.peak_accounted_bytes,"cross-request codec cache detected");}
    // Valid raw count replacement with unchanged total: the snapshot callback
    // must reject this before any sample plane writes. Samples stay compressed.
    fixture(p,points());independent_wire(p,false,true);
    auto chunks=inspect(p);const auto count_chunk=chunks.values.at(0);auto original=bytes(p);auto stamp=fs::last_write_time(p);bool mutated=false;
    reject("deep_decode_failed",[&]{read_deep_image(p,0,mapping(),{},[&](const char* phase){if(!mutated && std::string(phase)=="counts"){
        auto changed=original;changed[count_chunk.sample_count_data_offset]=1;save(p,changed);fs::last_write_time(p,stamp);mutated=true;
    }});});save(p,original);
    try{read_deep_image(p,0,mapping(),{},[&](const char* phase){if(std::string(phase)=="counts"){std::ofstream out(p,std::ios::app|std::ios::binary);out<<'x';}});throw std::runtime_error("change accepted");}catch(const FileFailure& e){need(e.code=="source_changed","source classification");}save(p,original);check_pixels(read_deep_image(p,0,mapping()));
    // Small payload stays a raw fallback even though native compression enum is 13.
    fixture(p,{points()[0]},false,false,false,1,1);auto raw=inspect(p);need(raw.raw_counts>0 && raw.raw_samples>0,"raw fallback not exercised");auto fallback=read_deep_image(p,0,mapping());need(fallback.pixels[1]==.75f && fallback.pixels[2]==.5f,"raw fallback pixels");
    std::string source_json,bg_json;for(auto s:deep_reference_json_parts)source_json+=s;for(auto s:deep_background_reference_json_parts)bg_json+=s;
    auto refs=nlohmann::json::parse(source_json),backgrounds=nlohmann::json::parse(bg_json);int decimal=0,bg=0;
    for(std::size_t i=0;i<refs["cases"].size();++i){const auto& item=refs["cases"][i];std::vector<DeepSample> samples;
        for(const auto& s:item["samples"])samples.push_back({s["z"],s["zback"],s["colour"].get<std::array<float,3>>(),s["alpha"].get<std::array<float,3>>()});
        fixture(p,samples,i%2==1);auto img=read_deep_image(p,0,mapping());
        for(int x=0;x<128;++x)for(int c=0;c<6;++c){float expected=item["expected"][c<3?"colour":"alpha"][c%3];need(std::abs(img.pixels[x*6+c]-expected)<=2e-6f*std::abs(expected)+std::numeric_limits<float>::denorm_min(),"Decimal ZSTD reference");}++decimal;
        for(std::size_t b=0;b<backgrounds["backgrounds"].size();++b){auto output=DeepReadOutput{DeepOutputMode::opaque_rgba,backgrounds["backgrounds"][b].get<std::array<double,3>>()};auto rgba=read_deep_image(p,0,mapping(),{}, {},output);
            for(int x=0;x<128;++x)for(int c=0;c<3;++c){float expected=backgrounds["cases"][i]["expected"][b][c];need(std::abs(rgba.pixels[x*4+c]-expected)<=2e-6f*std::max(1.f,std::abs(expected)),"Decimal ZSTD background");}++bg;}
    }
    fixture(p,points());independent_wire(p,true);check_pixels(read_deep_image(p,0,mapping()));
    independent_wire(p,false);check_pixels(read_deep_image(p,0,mapping()));
    independent_wire(p,true,false,true);check_pixels(read_deep_image(p,0,mapping()));
    auto infinite=points();infinite[0].z=infinite[0].zback=std::numeric_limits<float>::infinity();fixture(p,infinite);check_pixels(read_deep_image(p,0,mapping()));
    fixture(p,points());
    fixture(p,points());auto comp=inspect(p);auto original_v1=bytes(p);auto version2=original_v1;for(auto c:comp.values){version2[c.sample_count_data_offset+8]=2;version2[c.data_offset+8]=2;}save(p,version2);check_pixels(read_deep_image(p,0,mapping()));
    std::vector<std::future<DeepImage>> readers;for(int n=0;n<4;++n)readers.push_back(std::async(std::launch::async,[&]{return read_deep_image(p,0,mapping());}));for(auto& r:readers)check_pixels(r.get());
    std::cout<<nlohmann::json{{"bounded",true},{"fault_allocations",requests},{"layouts",layouts},{"decimal_cases",decimal},{"background_cases",bg},{"unknown_fds",true},{"positive_infinity",true},{"independent_v1_v2_delta",true},{"v2_without_delta",true},{"compressed_counts",wire.compressed_counts},{"compressed_samples",wire.compressed_samples}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

