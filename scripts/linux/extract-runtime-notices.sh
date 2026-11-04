#!/usr/bin/env bash
# Extract only verified CentOS package materials to a fresh, private cache path.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
rpm_file="$repo/.deps/linux/license-rpms/libgcc-4.8.5-44.el7.x86_64.rpm"
printf '%s  %s\n' cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa "$rpm_file" | sha256sum -c -
rpm -K "$rpm_file"
destination=${THUMBGEN_NOTICE_ROOT:-"$cache/runtime-license-rpm"}
if test -e "$destination"; then
    python3 "$repo/scripts/linux/verify-notice-materials.py" "$destination"
else
    mkdir "$destination"
    cd "$destination"
    rpm2cpio "$rpm_file" | cpio -idm --no-absolute-filenames
    python3 "$repo/scripts/linux/verify-notice-materials.py" "$destination" --create
fi
printf 'Verified runtime notices: %s\n' "$destination/usr/share/doc/libgcc-4.8.5"
