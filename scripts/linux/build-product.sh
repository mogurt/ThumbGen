#!/usr/bin/env bash
# Incremental product-only build using the already prepared CentOS SDK.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
logs=${THUMBGEN_BUILD_LOG_DIR:-"$cache/logs"}
mkdir -p "$logs"
python3 "$repo/scripts/linux/prepare-test-deps.py" --install
cmake -S "$repo" -B "$cache/thumbgen-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$cache/sdk;$cache/oiio-deps/dist" -DBUILD_TESTING=ON \
    -DTHUMBGEN_BUILD_PROBES=OFF -DTHUMBGEN_BUILD_VIPS=OFF -DTHUMBGEN_TEST_PROTOCOL=ON \
    -DPython3_EXECUTABLE="$cache/tools/python-3.11.11/bin/python3"
cmake --build "$cache/thumbgen-build" 2>&1 | tee "$logs/thumbgen-build.log"
export LD_LIBRARY_PATH="$cache/sdk/lib:$cache/oiio-deps/dist/lib:${LD_LIBRARY_PATH:-}"
"$cache/thumbgen-build/thumbgen" capabilities --json | tee "$logs/capabilities.json"
ctest --test-dir "$cache/thumbgen-build" --output-on-failure 2>&1 | tee "$logs/ctest.log"
python3 "$repo/scripts/linux/audit-abi.py" "$cache/thumbgen-build/thumbgen" \
    "$cache/thumbgen-build/thumbgen_core_tests" "$cache/thumbgen-build/thumbgen_image_matrix" \
    "$cache/thumbgen-build/thumbgen_budget_tests" "$cache/thumbgen-build/thumbgen_process_tests" \
    "$cache/thumbgen-build/thumbgen_control_tests" \
    "$cache/thumbgen-build/thumbgen_worker_protocol_tests" "$cache/thumbgen-build/thumbgen_isolated_tests" \
    > "$logs/abi.json"
