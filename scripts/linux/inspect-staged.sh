#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
stage="$repo/.deps/linux/validation"
label=${WSL_DISTRO_NAME:-linux}
export LD_LIBRARY_PATH="$stage/lib"
mkdir -p "$stage/results/$label"
for executable in thumbgen thumbgen_core_tests thumbgen_image_matrix; do
    ldd "$stage/bin/$executable" > "$stage/results/$label/loader-$executable.txt"
done
"$stage/bin/thumbgen" --version --json > "$stage/results/$label/version.json"
cd "$stage"
sha256sum bin/* lib/* tests/test_cli.py tests/ctest-suite.json > "results/$label/artifacts.sha256"
if test "$label" = CentOS-7.9; then
    /home/builder/thumbgen-cache/tools/python-3.11.11/bin/python3 "$repo/scripts/linux/audit-abi.py" \
        "$stage/bin/thumbgen" "$stage/bin/thumbgen_core_tests" "$stage/bin/thumbgen_image_matrix" > "$stage/results/$label/abi.json"
fi
