#!/usr/bin/env bash
# Run inside the pinned CentOS 7.9 release container.
set -eo pipefail
repo=${1:?source checkout required}
output=${2:?output directory required}
version=${3:?version required}
commit=${4:?commit required}
scripts=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=/tmp/thumbgen-release
mkdir -p "$work"

bash "$repo/scripts/linux/setup-centos7.sh"
yum -y install cpio
export THUMBGEN_LINUX_CACHE="$work/tools"
bash "$repo/scripts/linux/bootstrap-tools.sh"
source "$work/tools/environment.sh"
set -u
test "$(getconf GNU_LIBC_VERSION)" = 'glibc 2.17'

# CentOS 7's OpenSSL is too old for the bootstrapped Python HTTPS module.
# Keep the existing digest and RPM signature checks after the curl download.
export THUMBGEN_NOTICE_ROOT="$work/notices"
rpm_dir="$repo/.deps/linux/license-rpms"
mkdir -p "$rpm_dir"
curl -fL --retry 2 https://vault.centos.org/7.9.2009/os/x86_64/Packages/libgcc-4.8.5-44.el7.x86_64.rpm \
    -o "$rpm_dir/libgcc-4.8.5-44.el7.x86_64.rpm"
python3 "$repo/scripts/linux/fetch-runtime-notices.py"
bash "$repo/scripts/linux/extract-runtime-notices.sh"

# Bootstrap a NASM that supports the pinned SDK's JPEG SIMD sources.
nasm_archive="$work/nasm.tar.xz"
curl -fL --retry 2 https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz -o "$nasm_archive"
printf '%s  %s\n' 1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148 "$nasm_archive" | sha256sum -c -
tar -xJf "$nasm_archive" -C "$work"
(cd "$work/nasm-2.16.03" && ./configure --prefix="$work/tools" && make -j4 && make install)
export PATH="$work/tools/bin:$PATH"

git config --global --add safe.directory "$repo"
test "$(git -C "$repo" rev-parse HEAD)" = "$commit"
bash "$repo/scripts/ci/linux-sdk.sh"
sdk="$repo/.deps/ci-linux-sdk"
deps="$work/product-deps"
python3 "$repo/scripts/ci/product-deps.py" --sdk "$sdk" --output "$deps"

git -C "$repo" archive --format=tar --output="$work/source.tar" "$commit"
mkdir "$work/source"
tar -xf "$work/source.tar" -C "$work/source"
prefix="$deps/colour/sdk;$deps/bounded-openexr/prefix;$deps/portable-dwa/prefix;$sdk/sdk;$sdk/oiio-deps/dist"
cmake -S "$work/source" -B "$work/product" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF -DTHUMBGEN_VERIFY_DEPENDENCY_VERSIONS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    '-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic' "-DCMAKE_PREFIX_PATH=$prefix" \
    "-DOpenImageIO_DIR=$deps/portable-dwa/prefix/lib/cmake/OpenImageIO" \
    "-DCMAKE_INSTALL_PREFIX=$work/install" '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib'
cmake --build "$work/product" --parallel 4
cmake --install "$work/product"
python3 "$repo/scripts/linux/stage-runtime-product.py" --source "$work/source" --archive "$work/source.tar" \
    --build "$work/product" --install "$work/install" --cache "$sdk" --color-root "$deps/colour" \
    --core-root "$deps/bounded-openexr" --dwa-root "$deps/portable-dwa" --output "$work/stage" --cmake "$(command -v cmake)"
python3 "$repo/scripts/linux/package-runtime.py" --source "$work/source" --source-archive "$work/source.tar" \
    --validation "$work/stage" --cache "$sdk" --tools-cache "$work/tools" --notice-materials "$THUMBGEN_NOTICE_ROOT" \
    --color-root "$deps/colour" --core-root "$deps/bounded-openexr" --dwa-root "$deps/portable-dwa" \
    --product-build "$work/product" --output "$work/candidates"
candidates=("$work/candidates"/*.tar.gz)
test "${#candidates[@]}" -eq 1
python3 "$scripts/assemble.py" linux --source "$work/source" --install "$work/install" \
    --candidate "${candidates[0]}" --output "$output" --version "$version" --commit "$commit"
env -u LD_LIBRARY_PATH python3 "$scripts/verify.py" "$output/thumbgen-$version-linux-x86_64-glibc217.tar.gz" \
    --version "$version" --commit "$commit"
