#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
stage=${THUMBGEN_VALIDATION_DIR:-"$repo/.deps/linux/validation"}
python=${THUMBGEN_TEST_PYTHON:-python3}
if test -z "${THUMBGEN_TEST_PYTHON:-}"; then
    for candidate in /home/builder/thumbgen-cache/tools/python-3.11.11/bin/python3 "$HOME/thumbgen-test-tools/python-3.11.11/bin/python3"; do
        if test -x "$candidate"; then python="$candidate"; break; fi
    done
fi
label=${WSL_DISTRO_NAME:-linux}
export LD_LIBRARY_PATH="$stage/lib"
mkdir -p "$stage/results/$label"
cd "$stage/results/$label"
getconf GNU_LIBC_VERSION | tee platform.txt
cat /etc/os-release >> platform.txt
"$stage/bin/thumbgen" capabilities --json | tee capabilities.json
"$python" "$repo/scripts/linux/write-staged-tests.py" "$stage" "$python"
"$stage/tools/cmake/bin/ctest" --output-on-failure 2>&1 | tee ctest.log
