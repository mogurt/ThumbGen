#!/usr/bin/env bash
# Rebuild dependency libraries in a new cache; reuse only tools and Git source objects.
set -euo pipefail
cache=${1:?new SDK cache directory required}
tools_cache=${2:?existing compiler/tools/source cache required}
archive=${3:?verified project git archive required}
oiio_source=${4:?existing pinned OIIO source required}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$tools_cache/environment.sh"
mkdir "$cache"
mkdir -p "$cache/logs" "$cache/source-archive"
tar -xf "$archive" -C "$cache/source-archive"
test ! -e "$cache/source-archive/.git"
test ! -e "$cache/source-archive/.deps"
test ! -e "$cache/source-archive/build"
python3 "$script_dir/seed-sdk-sources.py" "$cache" "$tools_cache" "$oiio_source" "$cache/source-archive/scripts/linux/sources-lock.json" \
    > "$cache/logs/source-seed.log" 2>&1
printf 'source %q\n' "$tools_cache/environment.sh" > "$cache/environment.sh"
printf 'export THUMBGEN_LINUX_CACHE=%q\nexport THUMBGEN_LINUX_TOOLS_CACHE=%q\n' "$cache" "$tools_cache" >> "$cache/environment.sh"
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
export CC=gcc CXX=g++ CFLAGS='-march=x86-64 -mtune=generic' CXXFLAGS='-march=x86-64 -mtune=generic'
unset THUMBGEN_SDK_PREFIX CMAKE_LIBRARY_PATH CMAKE_INCLUDE_PATH OpenImageIO_ROOT nlohmann_json_ROOT
export CMAKE_PREFIX_PATH="$cache/oiio-deps/dist"
export PKG_CONFIG_PATH="$cache/oiio-deps/dist/lib/pkgconfig"
export LD_LIBRARY_PATH="$cache/sdk/lib:$cache/oiio-deps/dist/lib"
export GIT_TERMINAL_PROMPT=0 GIT_HTTP_LOW_SPEED_LIMIT=1000 GIT_HTTP_LOW_SPEED_TIME=30
prefix="$cache/sdk"
args=(-S "$cache/oiio-source" -B "$cache/oiio-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_IGNORE_PREFIX_PATH="$tools_cache/sdk;$tools_cache/oiio-deps/dist"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    -DOpenImageIO_LOCAL_DEPS_ROOT="$cache/oiio-deps" -DOIIO_LOCAL_DEPS_PATH="$cache/oiio-deps/dist"
    -DOpenImageIO_BUILD_MISSING_DEPS=all -DOpenImageIO_BUILD_LOCAL_DEPS=all
    '-DOpenImageIO_REQUIRED_DEPS=PNG;WebP'
    -DOpenImageIO_DEPENDENCY_BUILD_ALLOW_UNVERIFIED_TAGS=OFF
    -DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRIES=1 -DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRY_DELAY=2
    -DOIIO_BUILD_TOOLS=OFF -DOIIO_BUILD_TESTS=OFF -DBUILD_TESTING=OFF
    -DINSTALL_DOCS=OFF -DUSE_PYTHON=OFF -DUSE_QT=OFF -DUSE_SIMD=sse2 -DOIIO_USE_HWY=OFF -DSTOP_ON_WARNING=OFF)
for package in Freetype OpenCV TBB DCMTK FFmpeg GIF Libheif LibRaw OpenJPEG openjph OpenVDB Ptex R3DSDK Nuke OpenGL Qt5 Qt6 JXL libuhdr; do
    args+=("-DUSE_${package}=OFF")
done
cmake "${args[@]}" 2>&1 | tee "$cache/logs/oiio-configure.log"
cmake --build "$cache/oiio-build" 2>&1 | tee "$cache/logs/oiio-build.log"
cmake --install "$cache/oiio-build" 2>&1 | tee "$cache/logs/oiio-install.log"
cmake -S "$cache/src/json" -B "$cache/json-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$prefix" -DJSON_BuildTests=OFF \
    2>&1 | tee "$cache/logs/json-configure.log"
cmake --build "$cache/json-build" 2>&1 | tee "$cache/logs/json-build.log"
cmake --install "$cache/json-build" 2>&1 | tee "$cache/logs/json-install.log"
