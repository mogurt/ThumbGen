#!/usr/bin/env bash
# Modern-host CI SDK; this does NOT prove glibc 2.17 release compatibility.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
mkdir -p build/ci
mode=${1:-all}
case "$mode" in all|build) ;; *) echo 'Usage: linux-sdk.sh [all|build]' >&2; exit 2 ;; esac
# Fresh roots are mandatory. Never clean or mutate an existing SDK.
test ! -e .deps/ci-linux-sdk
for tool in git cmake ninja gcc g++ nasm python3; do command -v "$tool"; done
if test "$mode" = all; then
    test ! -e .deps/ci-linux-seed
    python3 scripts/ci/prepare-linux-sources.py 2>&1 | tee build/ci/public-sources.log
else
    # Resume only before SDK creation; the existing builder rechecks every HEAD
    # and reconstructs independent clones instead of compiling in this seed.
    test -f .deps/ci-linux-seed/public-sources.json
    test -f .deps/ci-linux-seed/environment.sh
fi
git archive --format=tar --output=build/ci/source.tar HEAD
bash scripts/linux/build-sdk.sh "$repo/.deps/ci-linux-sdk" "$repo/.deps/ci-linux-seed" \
    "$repo/build/ci/source.tar" "$repo/.deps/ci-linux-seed/oiio-source" \
    2>&1 | tee build/ci/linux-sdk.log
python3 scripts/linux/audit-sdk-build.py "$repo/.deps/ci-linux-sdk" "$repo/.deps/ci-linux-seed" \
    2>&1 | tee build/ci/linux-sdk-audit.log
