#!/usr/bin/env bash
# Test CPython ctypes support only; never install into a product stage/lib.
set -euo pipefail
source_library=${1:?verified CentOS libffi.so.6.0.1 required}
python=${2:?test Python executable required}
expected=af4ed7088fc6c9dd07628ba47534746412c051a4864189fb9308be5b8c1b2308
actual=$(sha256sum "$source_library")
test "${actual%% *}" = "$expected"
python_prefix=$("$python" -c 'import sys; print(sys.prefix)')
test "$python_prefix" != /usr
test "$python_prefix" != /usr/local
mkdir -p "$python_prefix/lib"
install -m 755 "$source_library" "$python_prefix/lib/libffi.so.6"
LD_LIBRARY_PATH="$python_prefix/lib" "$python" -c 'import ctypes; print("Test ctypes/libffi import OK")'
