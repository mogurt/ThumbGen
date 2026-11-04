#!/usr/bin/env bash
# Clone the pinned official source only when absent; never replace existing data.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${THUMBGEN_LINUX_CACHE:-/home/builder/thumbgen-cache}
source "$cache/environment.sh"
source_path="$repo/.deps/oiio-src"
expected=5c4408f545f1da5f30102c5f1c7151dbd9faf522
export GIT_TERMINAL_PROMPT=0
export GIT_HTTP_LOW_SPEED_LIMIT=1000 GIT_HTTP_LOW_SPEED_TIME=30
if test ! -e "$source_path"; then
    mkdir -p "$repo/.deps"
    git clone --depth 1 --single-branch --branch v3.2.1.2 \
        https://github.com/AcademySoftwareFoundation/OpenImageIO.git "$source_path"
fi
test -d "$source_path/.git" || { echo 'Existing OIIO path is not a Git clone; left unchanged' >&2; exit 1; }
actual=$(git -c safe.directory="$source_path" -C "$source_path" rev-parse HEAD)
test "$actual" = "$expected" || { echo "Existing OIIO commit differs: $actual; left unchanged" >&2; exit 1; }
# Windows-created checkouts may contain CRLF with a clean LF index. Normalize
# only for this read-side comparison; do not change files or Git configuration.
test -z "$(git -c safe.directory="$source_path" -c core.autocrlf=true -C "$source_path" status --porcelain)" || {
    echo 'OIIO working tree has changes; left unchanged' >&2; exit 1;
}
printf 'Verified OIIO v3.2.1.2 %s\n' "$actual"
