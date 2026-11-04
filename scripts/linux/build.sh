#!/usr/bin/env bash
# All dependency builds happen on the selected Linux host, never in the Windows SDK.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
export CC=gcc CXX=g++
export CFLAGS='-march=x86-64 -mtune=generic'
export CXXFLAGS='-march=x86-64 -mtune=generic'
export GIT_TERMINAL_PROMPT=0
export GIT_HTTP_LOW_SPEED_LIMIT=1000 GIT_HTTP_LOW_SPEED_TIME=30
oiio="$repo/.deps/oiio-src"
bash "$repo/scripts/linux/prepare-sources.sh"
mkdir -p "$cache/logs" "$cache/src"
prefix="$cache/sdk"
args=(-S "$oiio" -B "$cache/oiio-build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib
    -DOpenImageIO_LOCAL_DEPS_ROOT="$cache/oiio-deps"
    -DOIIO_LOCAL_DEPS_PATH="$cache/oiio-deps/dist"
    -DOpenImageIO_BUILD_MISSING_DEPS=all
    -DOpenImageIO_BUILD_LOCAL_DEPS=all
    '-DOpenImageIO_REQUIRED_DEPS=PNG;WebP'
    -DOpenImageIO_DEPENDENCY_BUILD_ALLOW_UNVERIFIED_TAGS=OFF
    -DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRIES=1
    -DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRY_DELAY=2
    -DOIIO_BUILD_TOOLS=OFF -DOIIO_BUILD_TESTS=OFF -DBUILD_TESTING=OFF
    -DINSTALL_DOCS=OFF -DUSE_PYTHON=OFF -DUSE_QT=OFF -DUSE_SIMD=sse2
    -DOIIO_USE_HWY=OFF -DSTOP_ON_WARNING=OFF)
for package in Freetype OpenCV TBB DCMTK FFmpeg GIF Libheif LibRaw OpenJPEG openjph OpenVDB Ptex R3DSDK Nuke OpenGL Qt5 Qt6 JXL libuhdr; do
    args+=("-DUSE_${package}=OFF")
done
cmake "${args[@]}" 2>&1 | tee "$cache/logs/oiio-configure.log"
cmake --build "$cache/oiio-build" 2>&1 | tee "$cache/logs/oiio-build.log"
cmake --install "$cache/oiio-build" 2>&1 | tee "$cache/logs/oiio-install.log"
# nlohmann_json is separately required by ThumbGen.
json="$cache/src/json"
test -d "$json/.git" || git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json "$json"
test "$(git -C "$json" rev-parse HEAD)" = 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
cmake -S "$json" -B "$cache/json-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$prefix" -DJSON_BuildTests=OFF
cmake --install "$cache/json-build"
bash "$repo/scripts/linux/build-product.sh"
