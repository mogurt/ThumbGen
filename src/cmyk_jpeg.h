#pragma once
#include <string>
namespace thumbgen::detail {
// Baseline Adobe CMYK/YCCK only. Output is normalized non-inverted ICC inks.
void read_cmyk_jpeg(const std::string& path,int width,int height,float* pixels);
}
