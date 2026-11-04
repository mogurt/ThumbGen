#!/usr/bin/env bash
# Build only an explicitly supplied git archive in a new private source directory.
set -euo pipefail
archive=${1:?git archive tar required}
source_dir=${2:?new private source directory required}
logs=${3:?new log directory required}
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
test ! -e "$logs"
mkdir "$source_dir"
mkdir -p "$logs"
tar -xf "$archive" -C "$source_dir"
test ! -e "$source_dir/.git"
test ! -e "$source_dir/.deps"
test ! -e "$source_dir/build"
export THUMBGEN_SDK_PREFIX="$cache/sdk;$cache/oiio-deps/dist"
export THUMBGEN_TEST_PYTHON="$cache/tools/python-3.11.11/bin/python3"
export LD_LIBRARY_PATH="$cache/sdk/lib:$cache/oiio-deps/dist/lib"
cd "$source_dir"
cmake --preset linux-dev > "$logs/configure-dev.log" 2>&1
cmake --build --preset linux-dev > "$logs/build-dev.log" 2>&1
OCIO=/thumbgen-intentionally-invalid-parent-config.ocio ctest --preset linux-dev > "$logs/ctest-dev.log" 2>&1
env -u THUMBGEN_TEST_PYTHON cmake --preset linux-release > "$logs/configure-release.log" 2>&1
env -u THUMBGEN_TEST_PYTHON cmake --build --preset linux-release > "$logs/build-release.log" 2>&1
cmake --build build/linux-release --target help > "$logs/targets-release.txt" 2>&1
build/linux-dev/thumbgen --version --json > "$logs/version-dev.json"
build/linux-release/thumbgen --version --json > "$logs/version-release.json"
python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$source_dir" > "$logs/source-dir.txt"
