#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
stage="$repo/.deps/linux/validation"
python=${THUMBGEN_TEST_PYTHON:-python3}
label=${WSL_DISTRO_NAME:-linux}
export LD_LIBRARY_PATH="$stage/lib"
mkdir -p "$stage/results/$label"
cd "$stage/results/$label"
getconf GNU_LIBC_VERSION | tee platform.txt
cat /etc/os-release >> platform.txt
"$stage/bin/thumbgen" capabilities --json | tee capabilities.json
"$python" "$repo/scripts/linux/write-staged-tests.py" "$stage" "$python"
"$stage/tools/cmake/bin/ctest" --output-on-failure 2>&1 | tee ctest.log
