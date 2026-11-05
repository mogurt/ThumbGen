#include <algorithm>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <OpenImageIO/imageio.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
using namespace OIIO;
using Json=nlohmann::ordered_json;
int main(int argc,char**argv){try {
 if(argc!=3)return 2;
 auto in=ImageInput::open(argv[1]);if(!in)throw std::runtime_error(geterror());auto s=in->spec();
 Json j={{"oiio_version",OIIO_VERSION_STRING},{"input",argv[1]},{"width",s.width},{"height",s.height},{"x",s.x},{"y",s.y},{"format",s.format.c_str()},{"compression",s.get_string_attribute("compression")},{"channels",s.channelnames},{"reads",Json::array()}};
 j["metadata_xml"]=s.to_xml();for(auto name:{"buildinfo","library_list","simd","hw:simd"})j[name]=get_string_attribute(name);
 for(int c=0;c<s.nchannels;++c)j["channel_types"].push_back(s.channelformats.empty()?s.format.c_str():s.channelformats[c].c_str());
 std::vector<int> channels;for(const auto& name:{"id_a.R","id_a.G","id_a.B"}){auto it=std::find(s.channelnames.begin(),s.channelnames.end(),name);if(it==s.channelnames.end())throw std::runtime_error("missing channel");channels.push_back(int(it-s.channelnames.begin()));}
 if(channels[1]!=channels[0]+1||channels[2]!=channels[0]+2)throw std::runtime_error("noncontiguous RGB");
 for(auto format:{TypeDesc::UNKNOWN,TypeDesc::HALF,TypeDesc::FLOAT,TypeDesc::DOUBLE})for(int component=-1;component<3;++component){
  int begin=channels[component<0?0:component],end=component<0?channels[2]+1:begin+1;
  TypeDesc type(format);size_t stride=type==TypeDesc::UNKNOWN?s.pixel_bytes(begin,end,true):type.size()*(end-begin);std::vector<unsigned char> data(size_t(s.width)*s.height*stride);
  if(!in->read_image(0,0,begin,end,type,data.data()))throw std::runtime_error(in->geterror());
  std::string name=std::string(type.c_str())+"-"+(component<0?"rgb":std::to_string(component))+".bin";
  std::ofstream file(std::string(argv[2])+"/"+name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());if(!file)throw std::runtime_error("write failed");
  j["reads"].push_back({{"format",type.c_str()},{"component",component},{"begin",begin},{"end",end},{"stride",stride},{"file",name},{"bytes",data.size()}});
 }
 in->close();
#if defined(_WIN32)
 HMODULE mods[1024];DWORD needed=0;if(!EnumProcessModules(GetCurrentProcess(),mods,sizeof(mods),&needed))throw std::runtime_error("modules query");
 for(unsigned i=0;i<needed/sizeof(HMODULE);++i){char path[32768];if(GetModuleFileNameA(mods[i],path,sizeof(path)))j["loaded_modules"].push_back(path);}
#else
 std::ifstream maps("/proc/self/maps");j["proc_maps"]=std::string(std::istreambuf_iterator<char>(maps),{});
#endif
 std::cout<<j.dump(2)<<'\n';return 0;
 }catch(std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
