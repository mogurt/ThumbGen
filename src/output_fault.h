#pragma once
#include <OpenImageIO/imagebuf.h>
#include <cstddef>

namespace thumbgen::detail {
// Only the separate test-core target enables these hooks. No environment,
// task input, protocol field or production runtime switch controls injection.
#ifdef THUMBGEN_OUTPUT_FAULT_TESTS
using OutputFaultHook=bool(*)(const char*,std::size_t);
inline thread_local OutputFaultHook output_fault_hook=nullptr;
inline thread_local std::size_t output_fault_current=0;
inline void output_fault_index(std::size_t index){output_fault_current=index;}
inline std::size_t output_fault_index(){return output_fault_current;}
inline bool output_fault(const char* phase,std::size_t index){return output_fault_hook && output_fault_hook(phase,index);}
#else
inline void output_fault_index(std::size_t){}
inline std::size_t output_fault_index(){return 0;}
inline bool output_fault(const char*,std::size_t){return false;}
#endif
inline OIIO::ImageBuf preview_storage(const OIIO::ImageSpec& spec){
    // Deterministic allocation-boundary witness: throw bad_alloc or return an
    // empty buffer in the test target. Production performs the original ctor.
    if(output_fault("preview",output_fault_index()))return OIIO::ImageBuf{};
    return OIIO::ImageBuf(spec);
}
}
