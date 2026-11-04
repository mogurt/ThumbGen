#!/usr/bin/env bash
set -euo pipefail
test "$(id -un)" = builder
grep -q 'thumbgen-cache/environment.sh' "$HOME/.bashrc" || printf '\n[ ! -f /home/builder/thumbgen-cache/environment.sh ] || source /home/builder/thumbgen-cache/environment.sh\n' >> "$HOME/.bashrc"
source /home/builder/thumbgen-cache/environment.sh
id
g++ --version | head -1
git --version
cmake --version | head -1
ninja --version
python3 --version
getconf GNU_LIBC_VERSION
