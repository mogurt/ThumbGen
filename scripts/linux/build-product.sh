#!/usr/bin/env bash
# Incremental product-only build using the already prepared CentOS SDK.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
mkdir -p "$cache/logs"
cmake -S "$repo" -B "$cache/thumbgen-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$cache/sdk;$cache/oiio-deps/dist" -DBUILD_TESTING=ON \
    -DTHUMBGEN_BUILD_PROBES=OFF -DTHUMBGEN_BUILD_VIPS=OFF \
    -DPython3_EXECUTABLE="$cache/tools/python-3.11.11/bin/python3"
cmake --build "$cache/thumbgen-build" 2>&1 | tee "$cache/logs/thumbgen-build.log"
export LD_LIBRARY_PATH="$cache/sdk/lib:$cache/oiio-deps/dist/lib:${LD_LIBRARY_PATH:-}"
"$cache/thumbgen-build/thumbgen" capabilities --json | tee "$cache/logs/capabilities.json"
ctest --test-dir "$cache/thumbgen-build" --output-on-failure 2>&1 | tee "$cache/logs/ctest.log"
python3 "$repo/scripts/linux/audit-abi.py" "$cache/thumbgen-build/thumbgen" \
    "$cache/thumbgen-build/thumbgen_core_tests" "$cache/thumbgen-build/thumbgen_image_matrix" \
    > "$cache/logs/abi.json"
