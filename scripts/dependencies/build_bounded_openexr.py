"""Build an isolated, pinned OpenEXR with request-owned ZSTD decoding.

Never modifies the supplied source cache or dependency prefix. Run on Windows in
an MSVC developer shell for Ninja, or use the default Visual Studio generator.
On Linux, source the locked GCC toolchain environment before invoking this file.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

COMMIT = '5d838e01770cadb8f2a36c0d17018452fa7a3c0c'
IDENTITY = '-dev-thumbgen-bounded-zstd-v1'
PATCH = Path(__file__).resolve().parents[2] / 'dependencies/patches/openexr-bounded-zstd-v1.patch'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    helper_sha256 = digest(Path(__file__))
    patch_sha256 = digest(PATCH)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-cache', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path, help='new private directory; must not exist')
    p.add_argument('--deps-prefix', required=True, type=Path)
    p.add_argument('--generator', default='Visual Studio 17 2022' if os.name == 'nt' else 'Ninja')
    p.add_argument('--shared', action='store_true', help='match the SDK library linkage (default static)')
    p.add_argument('--lib-suffix', default='_v_3_5_0_OpenImageIO_v3_2')
    p.add_argument('--imf-namespace', default='OpenImageIO_v3_2_Imf__3_5_0')
    a = p.parse_args()
    source_cache = a.source_cache.resolve(strict=True)
    deps_prefix = a.deps_prefix.resolve(strict=True)
    output = a.output.resolve()
    # Fresh directories prevent accidental in-place SDK replacement or reuse of stale binaries.
    if output.exists():
        p.error('--output must be a new private directory')
    if any(parent == output or output in parent.parents or parent in output.parents for parent in (source_cache, deps_prefix)):
        p.error('--output must not contain a read-only input')
    subprocess.run(['git', '-C', str(source_cache), 'cat-file', '-e', COMMIT + '^{commit}'], check=True)
    output.mkdir(parents=True)
    commands = []

    def run(name, args, cwd=None):
        commands.append(dict(name=name, args=list(map(str, args))))
        with (output / (name + '.log')).open('wb') as log:
            result = subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'{name} failed with original exit {result.returncode}; see {log.name}')

    source, build, prefix = (output / name for name in ('source', 'build', 'prefix'))
    run('clone', ['git', 'clone', '--no-hardlinks', '--no-checkout', str(source_cache), str(source)])
    run('checkout', ['git', '-C', str(source), '-c', 'core.autocrlf=false', 'checkout', '--detach', COMMIT])
    run('patch-check', ['git', '-C', str(source), 'apply', '--check', str(PATCH)])
    run('patch', ['git', '-C', str(source), 'apply', str(PATCH)])
    args = ['cmake', '-S', str(source), '-B', str(build), '-G', a.generator,
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + prefix.as_posix(),
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_PREFIX_PATH=' + deps_prefix.as_posix(),
            '-DBUILD_SHARED_LIBS=' + ('ON' if a.shared else 'OFF'),
            '-DBUILD_TESTING=OFF', '-DOPENEXR_BUILD_TOOLS=OFF', '-DOPENEXR_INSTALL_TOOLS=OFF',
            '-DOPENEXR_BUILD_EXAMPLES=OFF', '-DOPENEXR_BUILD_PYTHON=OFF',
            '-DOPENEXR_INSTALL_PKG_CONFIG=OFF', '-DOPENEXR_FORCE_INTERNAL_DEFLATE=ON',
            '-DOPENEXR_LIB_SUFFIX=' + a.lib_suffix, '-DOPENEXR_INTERNAL_IMF_NAMESPACE=' + a.imf_namespace,
            '-DOPENEXR_NAMESPACE_CUSTOM=1', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
    if os.name == 'nt' and a.generator.startswith('Visual Studio'):
        args += ['-A', 'x64']
    elif os.name != 'nt':
        args += ['-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic',
                 '-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic']
    run('configure', args)
    run('build', ['cmake', '--build', str(build), '--config', 'Release', '--parallel', '2'])
    run('install', ['cmake', '--install', str(build), '--config', 'Release'])
    changed = ['src/lib/OpenEXRCore/base.c', 'src/lib/OpenEXRCore/internal_zstd.c']
    receipt = dict(upstream=COMMIT, runtime_identity=IDENTITY, patch_sha256=patch_sha256,
                   helper_sha256=helper_sha256, commands=commands,
                   source_sha256={x: digest(source / x) for x in changed},
                   installed_sha256={x.relative_to(prefix).as_posix(): digest(x)
                                     for x in sorted(prefix.rglob('*')) if x.is_file()})
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(dict(prefix=str(prefix), receipt=str(output / 'receipt.json'))))


if __name__ == '__main__':
    main()
