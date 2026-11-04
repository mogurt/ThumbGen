#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
destination=${1:?new fixture directory required}
g++ -std=c++17 -march=x86-64 -mtune=generic "$repo/scripts/linux/package-fixtures.cpp" \
    -I"$cache/sdk/include" -I"$cache/oiio-deps/dist/include" \
    -L"$cache/sdk/lib" -Wl,-rpath,"$cache/sdk/lib" -lOpenImageIO -lOpenImageIO_Util \
    -o "$cache/package-fixtures"
"$cache/package-fixtures" "$destination"
chmod a-w "$destination"/*
sha256sum "$destination"/*
