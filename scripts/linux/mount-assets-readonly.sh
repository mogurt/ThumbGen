#!/usr/bin/env bash
set -euo pipefail
test "$(id -u)" = 0
source_path=$(realpath "${1:?source directory required}")
target_path=${2:-/mnt/thumbgen-assets}
test -d "$source_path"
if mountpoint -q "$target_path"; then
    findmnt --target "$target_path"
    echo 'Existing mount left unchanged; inspect before reuse.' >&2
    exit 1
fi
mkdir -p "$target_path"
test -z "$(ls -A "$target_path")"
mount --bind "$source_path" "$target_path"
mount -o remount,bind,ro "$target_path"
findmnt --target "$target_path"
findmnt -n -o OPTIONS --target "$target_path" | grep -Eq '(^|,)ro(,|$)'
