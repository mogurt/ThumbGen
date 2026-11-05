#!/usr/bin/env bash
# Optional local validation helper; hosted CI uses its provisioned host tools.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
root="$repo/.deps/ci-linux-tools"
mkdir -p build/ci
python3 scripts/ci/fetch-local-tools.py | tee build/ci/local-tools-hashes.log
test ! -e "$root/nasm-source"
test ! -e "$root/nasm"
mkdir "$root/nasm-source"
tar -xf "$root/downloads/nasm-2.16.03.tar.xz" -C "$root/nasm-source" --strip-components=1
python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "$root/downloads/ninja-linux.zip" "$root/bin"
chmod +x "$root/bin/ninja"
(
    cd "$root/nasm-source"
    ./configure --prefix="$root/nasm"
    make -j2
    make install
) > build/ci/local-nasm-build.log 2>&1
# Debian may omit ensurepip. Bootstrap only this venv from the hash-locked wheel.
python3 -m venv --without-pip .deps/ci-linux-python
PYTHONPATH="$root/downloads/pip-26.3-py3-none-any.whl" \
    .deps/ci-linux-python/bin/python -m pip install --no-cache-dir --no-index \
    "$root/downloads/pip-26.3-py3-none-any.whl" > build/ci/local-pip-bootstrap.log 2>&1
.deps/ci-linux-python/bin/python -m pip install --no-cache-dir -r tests/requirements.txt \
    > build/ci/linux-test-dependencies.log 2>&1
"$root/bin/ninja" --version
"$root/nasm/bin/nasm" -v
.deps/ci-linux-python/bin/python --version
