#!/usr/bin/env bash
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source /opt/rh/devtoolset-11/enable
test ! -f /opt/rh/rh-git227/enable || source /opt/rh/rh-git227/enable
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
mkdir -p "$cache/tools" "$cache/downloads" "$cache/src"
cd "$cache/downloads"
fetch() { test -f "$2" || curl -fL --connect-timeout 10 --max-time 180 --retry 1 "$1" -o "$2"; }
fetch https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-linux-x86_64.tar.gz cmake-3.31.8-linux-x86_64.tar.gz
fetch https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-SHA-256.txt cmake-3.31.8-SHA-256.txt
grep ' cmake-3.31.8-linux-x86_64.tar.gz$' cmake-3.31.8-SHA-256.txt | sha256sum -c -
tar -xzf cmake-3.31.8-linux-x86_64.tar.gz -C "$cache/tools"
fetch https://www.python.org/ftp/python/3.11.11/Python-3.11.11.tar.xz Python-3.11.11.tar.xz
fetch https://github.com/ninja-build/ninja/archive/refs/tags/v1.12.1.tar.gz ninja-1.12.1.tar.gz
sha256sum -c "$script_dir/tools.sha256"
sha256sum ./*.tar.* > "$cache/downloads/tool-downloads.sha256"
cd "$cache/src"
test -d Python-3.11.11 || tar -xf "$cache/downloads/Python-3.11.11.tar.xz"
cd Python-3.11.11
test -f Makefile || ./configure --prefix="$cache/tools/python-3.11.11" --without-ensurepip
make -j4
make install
export PATH="$cache/tools/python-3.11.11/bin:$cache/tools/cmake-3.31.8-linux-x86_64/bin:$PATH"
cd "$cache/src"
test -d ninja-1.12.1 || tar -xf "$cache/downloads/ninja-1.12.1.tar.gz"
cd ninja-1.12.1
python3 configure.py --bootstrap
mkdir -p "$cache/tools/bin"
cp ninja "$cache/tools/bin/"
cat > "$cache/environment.sh" <<EOF
source /opt/rh/devtoolset-11/enable
source /opt/rh/rh-git227/enable
export PATH="$cache/tools/bin:$cache/tools/python-3.11.11/bin:$cache/tools/cmake-3.31.8-linux-x86_64/bin:\$PATH"
export CC=gcc CXX=g++
export CFLAGS='-march=x86-64 -mtune=generic'
export CXXFLAGS='-march=x86-64 -mtune=generic'
EOF
source "$cache/environment.sh"
grep -q 'thumbgen-cache/environment.sh' /home/builder/.bashrc || printf '\n[ ! -f /home/builder/thumbgen-cache/environment.sh ] || source /home/builder/thumbgen-cache/environment.sh\n' >> /home/builder/.bashrc
g++ --version | head -1
cmake --version | head -1
ninja --version
python3 --version
getconf GNU_LIBC_VERSION
cat > "$cache/cxx17-smoke.cpp" <<'EOF'
#include <filesystem>
#include <iostream>
#include <optional>
int main() { std::optional<int> value=17; std::cout << "C++" << *value << " " << std::filesystem::current_path() << '\n'; }
EOF
g++ -std=c++17 -march=x86-64 -mtune=generic "$cache/cxx17-smoke.cpp" -o "$cache/cxx17-smoke"
"$cache/cxx17-smoke"
