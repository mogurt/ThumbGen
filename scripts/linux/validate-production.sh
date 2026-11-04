#!/usr/bin/env bash
# Keep mount and converter in the same WSL session/mount namespace.
set -euo pipefail
test "$(id -u)" = 0
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
manifest=${1:?manifest required}
asset_root=${2:?asset root required}
readonly_root=/mnt/thumbgen-assets
if ! mountpoint -q "$readonly_root"; then
    bash "$script_dir/mount-assets-readonly.sh" "$asset_root" "$readonly_root"
fi
test_user=${3:?test user required}
python=${4:-python3}
runuser -u "$test_user" -- "$python" "$script_dir/production-smoke.py" \
    --manifest "$manifest" --asset-root "$asset_root" --readonly-root "$readonly_root"
