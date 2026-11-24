#include "other_rows_internal.h"

namespace thumbgen::detail::other_rows {
namespace {
OIIO::ImageInput::unique_ptr open(const std::string& path,const std::string& format,
    const OIIO::ImageSpec& config,OIIO::ImageSpec& spec,const char* code) {
    // Instantiate only the codec that was preflighted, including when the
    // extension names another reader or when the plugin rejects the pixels.
    auto input=OIIO::ImageInput::create(format);
    if(!input)throw FileFailure(code,OIIO::geterror());
    if(!input->open(path,spec,config))throw FileFailure(code,input->geterror());
    return input;
}

class PluginRows final : public NativeRowSource {
public:
    PluginRows(const std::string& path,const std::string& format,const OIIO::ImageSpec& config,
        const OIIO::ImageSpec& planned) {
        OIIO::ImageSpec actual;
        input_=open(path,format,config,actual,"decode_failed");
        if(actual.width!=planned.width || actual.height!=planned.height || actual.format!=planned.format ||
           actual.channelnames!=planned.channelnames || actual.x!=planned.x || actual.y!=planned.y ||
           actual.get_int_attribute("Orientation",1)!=planned.get_int_attribute("Orientation",1) ||
           actual.get_int_attribute("oiio:UnassociatedAlpha",0)!=planned.get_int_attribute("oiio:UnassociatedAlpha",0))
            throw FileFailure("decode_failed","native layout changed after admission");
        height_=actual.height;
        y_=actual.y;
        stride_=actual.scanline_bytes();
    }
    void read(int first,int last,void* destination) override {
        if(first!=next_ || first>=last || last>height_)
            throw FileFailure("decode_failed","native rows must be read once in order");
        auto* out=static_cast<Byte*>(destination);
        // Intentionally call the single-row API. Several native bulk-read
        // implementations allocate a temporary sized by the whole request.
        for(;next_<last;++next_,out+=stride_)
            if(!input_->read_native_scanline(0,0,y_+next_,0,out))
                throw FileFailure("decode_failed",input_->geterror());
    }
    void finish() override {
        if(next_!=height_ || !input_->close())
            throw FileFailure("decode_failed","incomplete native image decode");
    }
private:
    OIIO::ImageInput::unique_ptr input_;
    int next_=0,height_=0,y_=0;
    std::size_t stride_=0;
};
}

OIIO::ImageSpec metadata(const Source& source,const char* format) {
    OIIO::ImageSpec spec;
    auto input=open(source.path,format,source.config,spec,"open_failed");
    return spec;
}

OtherRowsProbe supported(const Source& source,const char* format,OIIO::ImageSpec spec,
    std::uint64_t retained,std::uint64_t codec) {
    const auto complete_codec=checked_bytes_add(codec,retained);
    reserve(complete_codec,source.budget,"native frame and codec scratch exceed decode budget");
    // Capture only the layout. Avoid duplicating copied ICC and user metadata
    // in the deferred factory or each adapter object.
    OIIO::ImageSpec layout=spec;
    layout.extra_attribs.clear();
    layout.attribute("Orientation",spec.get_int_attribute("Orientation",1));
    layout.attribute("oiio:UnassociatedAlpha",spec.get_int_attribute("oiio:UnassociatedAlpha",0));
    NativeReadPlan plan{unsigned(spec.pixel_bytes()),retained,codec,
        [path=source.path,format=std::string(format),config=source.config,layout=std::move(layout)] {
            return std::make_unique<PluginRows>(path,format,config,layout);
        }};
    return {format,std::move(spec),std::move(plan),complete_codec};
}

OtherRowsProbe fallback(const Source& source,const char* format,std::uint64_t pixels,
    std::uint64_t codec,std::uint64_t eager) {
    // An unusual layout must fit both the audited plugin workspace and the
    // existing complete-image route. Eager allocations before scheduling
    // must also fit the bounded metadata envelope.
    reserve(eager,eager_limit,"unsupported layout requires a large decoder allocation before admission");
    reserve(checked_bytes_add(checked_bytes_mul(pixels,32),codec),source.budget,
        "unsupported layout requires a complete image decode that exceeds decode budget");
    return {format,std::nullopt,{},codec};
}
}

namespace thumbgen::detail {
std::optional<OtherRowsProbe> probe_other_rows(const std::string& path,std::uint64_t file_bytes,
    std::uint64_t budget,const OIIO::ImageSpec& config,std::size_t icc_limit) {
    using namespace other_rows;
    Source source(path,file_bytes,budget,config);
    if(!source.file)return std::nullopt;
    std::array<Byte,12> magic{};
    source.read(0,magic.data(),std::size_t(std::min<std::uint64_t>(file_bytes,magic.size())),"truncated image signature");
    if(file_bytes>=12 && is(magic.data(),"RIFF") && is(magic.data()+8,"WEBP"))
        return webp(source,icc_limit);
    if(file_bytes>=4 && (is(magic.data(),"SDPX") || is(magic.data(),"XPDS")))return dpx(source);
    if(file_bytes>=2 && (is(magic.data(),"BM",2) || is(magic.data(),"BA",2) ||
        is(magic.data(),"CI",2) || is(magic.data(),"CP",2) || is(magic.data(),"PT",2)))
        return bmp(source);
    if(file_bytes>=2 && is(magic.data(),"#?",2))return hdr(source);
    // TGA 1.0 has no reliable signature. Use the known suffixes and the
    // set of header layouts recognized by the plugin.
    auto extension=std::filesystem::u8path(path).extension().u8string();
    for(auto& c:extension)if(c>='A' && c<='Z')c=char(c-'A'+'a');
    const bool tga_suffix=extension==".tga" || extension==".tpic";
    if(tga_suffix && file_bytes<18)return targa(source);
    if(file_bytes>=18 && magic[1]<=1 && ((magic[2]>=1 && magic[2]<=3) || (magic[2]>=9 && magic[2]<=11))) {
        std::array<Byte,6> layout{};
        source.read(12,layout.data(),layout.size(),"truncated TGA layout");
        const auto bits=layout[4];
        if(tga_suffix || (le16(layout.data()) && le16(layout.data()+2) &&
            (bits==8 || bits==15 || bits==16 || bits==24 || bits==32)))return targa(source);
    }
    if(file_bytes>=26) {
        std::array<Byte,18> footer{};
        source.read(file_bytes-footer.size(),footer.data(),footer.size(),"truncated image footer");
        if(is(footer.data(),"TRUEVISION-XFILE.",18))return targa(source);
    }
    return std::nullopt;
}
}
