#!/usr/bin/env bash
set -euo pipefail
source /opt/rh/devtoolset-11/enable
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
mkdir -p "$cache"
cat > "$cache/cxx17-smoke.cpp" <<'EOF'
#include <filesystem>
#include <iostream>
#include <optional>
int main() { std::optional<int> value=17; std::cout << "C++" << *value << " " << std::filesystem::current_path() << '\n'; }
EOF
g++ -std=c++17 -march=x86-64 -mtune=generic "$cache/cxx17-smoke.cpp" -o "$cache/cxx17-smoke"
"$cache/cxx17-smoke"
g++ --version | head -1
getconf GNU_LIBC_VERSION
ldd "$cache/cxx17-smoke"
readelf --version-info "$cache/cxx17-smoke"
