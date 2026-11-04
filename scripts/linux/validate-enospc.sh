#!/usr/bin/env bash
# Root only creates an isolated mount; the converter runs unprivileged.
set -euo pipefail
test "$(id -u)" = 0
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_user=${1:?test user required}
python=${2:?Python interpreter required}
executable=${3:?ThumbGen executable required}
report=${4:?report path required}
test_uid=$(id -u "$test_user")
test_gid=$(id -g "$test_user")
test "$test_uid" != 0
mount_dir=$(mktemp -d /tmp/thumbgen-enospc.XXXXXXXX)
test "$(realpath "$mount_dir")" = "$mount_dir"
case "$mount_dir" in /tmp/thumbgen-enospc.*) ;; *) exit 1 ;; esac
cleanup() {
    if mountpoint -q "$mount_dir"; then umount -- "$mount_dir"; fi
    # No recursive removal: only the unique mountpoint this invocation made.
    rmdir -- "$mount_dir"
    printf 'Removed only this invocation\047s private tmpfs mountpoint: %s\n' "$mount_dir"
}
trap cleanup EXIT
mount -t tmpfs -o "size=4m,nr_inodes=256,mode=0700,uid=$test_uid,gid=$test_gid" thumbgen-enospc "$mount_dir"
runuser -u "$test_user" -- "$python" "$script_dir/test-enospc.py" \
    --mount "$mount_dir" --exe "$executable" --report "$report"
