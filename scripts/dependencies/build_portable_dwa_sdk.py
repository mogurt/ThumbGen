"""Build only locked OpenEXR/OIIO in a new private Linux prefix.

Existing source repositories, SDKs and dependency prefixes remain read-only.
The flat OIIO library embeds the patched static Core; this is independent of
the request-budget/ZSTD Core linked by the product's Deep reader.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / 'scripts/linux/sources-lock.json'
PATCH = ROOT / 'dependencies/patches/openexr-portable-dwa-v1.patch'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): digest(p)
            for p in sorted(root.rglob('*')) if p.is_file()}


def git(cache, *args):
    env = dict(os.environ, GIT_OPTIONAL_LOCKS='0')
    return subprocess.check_output(['git', '-C', str(cache), *args], env=env)


def verify_cache(cache, commit):
    if git(cache, 'rev-parse', 'HEAD').decode().strip() != commit:
        raise ValueError(f'{cache}: HEAD does not match the public lock')
    if (git(cache, 'diff', '--name-only', commit) or
            git(cache, 'ls-files', '--others')):
        raise ValueError(f'{cache}: changed or additional source files')
    return git(cache, 'ls-tree', '-r', '-z', commit)


def output_location(output, readonly, root):
    output = Path(output).resolve()
    root = Path(root).resolve()
    if output.exists():
        raise ValueError('--output must not exist')
    for source in readonly:
        source = Path(source).resolve(strict=True)
        if output == source or output in source.parents or source in output.parents:
            raise ValueError('--output must be separate from read-only inputs/source')
    # CI intentionally keeps fresh build outputs in the checkout's .deps.
    # Compare resolved targets to the literal private directory so a .deps
    # symlink into tracked source cannot authorize source-tree writes.
    if output == root or output in root.parents or (
            root in output.parents and root / '.deps' not in output.parents):
        raise ValueError('--output inside a checkout must be beneath .deps')
    return output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--openexr-cache', required=True, type=Path)
    p.add_argument('--oiio-cache', required=True, type=Path)
    p.add_argument('--deps-prefix', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    if sys.platform != 'linux':
        p.error('This portable x64/GCC recipe currently requires Linux')
    caches = {name: path.resolve(strict=True) for name, path in
              [('OpenEXR', a.openexr_cache), ('OpenImageIO', a.oiio_cache)]}
    deps = a.deps_prefix.resolve(strict=True)
    try:
        output = output_location(a.output, [*caches.values(), deps], ROOT)
    except ValueError as error:
        p.error(str(error))
    lock = json.loads(LOCK.read_text())
    exr = next(x for x in lock['dependencies'] if x['package'] == 'OpenEXR')
    if exr['version'] != '3.5.0' or lock['oiio_tag'] != 'v3.2.1.2':
        p.error('This patch/recipe requires locked OpenEXR3.5.0/OIIO3.2.1.2')
    commits = {'OpenEXR': exr['commit'], 'OpenImageIO': lock['oiio_commit']}
    trees = {name: verify_cache(cache, commits[name]) for name, cache in caches.items()}
    before = inventory(deps)
    output.mkdir(parents=True)
    prefix = output / 'prefix'
    receipt = dict(success=False, helper_sha256=digest(Path(__file__)),
                   lock_sha256=digest(LOCK), patch_sha256=digest(PATCH),
                   inputs=dict(lock=lock, caches={k: str(v) for k, v in caches.items()},
                               deps_prefix=str(deps)), commands=[], components={})

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(label, args):
        command = list(map(str, args))
        record = dict(label=label, command=command)
        receipt['commands'].append(record)
        with (output / (label + '.stdout')).open('wb') as stdout, \
                (output / (label + '.stderr')).open('wb') as stderr:
            r = subprocess.run(command, stdout=stdout, stderr=stderr,
                               env=dict(os.environ, GIT_OPTIONAL_LOCKS='0'))
        record['exit'] = r.returncode
        save()
        if r.returncode:
            raise RuntimeError(f'{label} exited {r.returncode}; original logs in {output}')

    def build(name, extra):
        source, binary = output / (name + '-source'), output / (name + '-build')
        run(name + '-clone', ['git', 'clone', '--no-hardlinks', '--no-checkout', caches[name], source])
        run(name + '-checkout', ['git', '-C', source, '-c', 'core.autocrlf=false',
                                'checkout', '--detach', commits[name]])
        if name == 'OpenEXR':
            run('patch-check', ['git', '-C', source, 'apply', '--check', PATCH])
            run('patch-apply', ['git', '-C', source, 'apply', PATCH])
        tree_path = output / (name + '-upstream-tree.bin')
        tree_path.write_bytes(trees[name])
        receipt['components'][name] = dict(commit=commits[name],
            git_tree_sha256=digest(tree_path), source=str(source), build=str(binary),
            source_files=inventory_without_git(source))
        args = ['cmake', '-S', source, '-B', binary, '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
                '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                '-DCMAKE_PREFIX_PATH=' + str(prefix) + ';' + str(deps),
                '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
                '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
                '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                '-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic',
                '-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic',
                '-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON', '-DCMAKE_INSTALL_RPATH=$ORIGIN',
                '-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF', *extra]
        run(name + '-configure', args)
        run(name + '-build', ['cmake', '--build', binary, '--parallel', '2', '--verbose'])
        run(name + '-install', ['cmake', '--install', binary])
        run(name + '-rules', ['ninja', '-C', binary, '-t', 'commands'])
        bindings = {}
        for line in (binary / 'install_manifest.txt').read_text().splitlines():
            path = Path(line)
            if not path.is_file() or path.is_symlink():
                continue
            if '.so' not in path.name and path.suffix != '.a':
                continue
            matches = [x for x in binary.rglob(path.name) if x.is_file() and not x.is_symlink()]
            matches = [x for x in matches if digest(x) == digest(path)]
            if not matches:
                raise ValueError(f'{name}: no exact build/install binding for {path}')
            bindings[str(path)] = dict(build=str(matches[0]), sha256=digest(path))
        receipt['components'][name]['build_install_bindings'] = bindings

    try:
        for tool in ('cmake', 'ninja', 'gcc', 'g++', 'ld'):
            run('tool-' + tool.replace('+', 'x'), [tool, '--version'])
        build('OpenEXR', [
            '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF',
            '-DOPENEXR_BUILD_TOOLS=OFF', '-DOPENEXR_INSTALL_TOOLS=OFF',
            '-DOPENEXR_BUILD_EXAMPLES=OFF', '-DOPENEXR_BUILD_PYTHON=OFF',
            '-DOPENEXR_INSTALL_PKG_CONFIG=OFF', '-DOPENEXR_FORCE_INTERNAL_DEFLATE=ON',
            '-DOPENEXR_LIB_SUFFIX=_v_3_5_0_OpenImageIO_v3_2',
            '-DOPENEXR_INTERNAL_IMF_NAMESPACE=OpenImageIO_v3_2_Imf__3_5_0',
            '-DOPENEXR_NAMESPACE_CUSTOM=1',
            '-DILMTHREAD_INTERNAL_NAMESPACE=OpenImageIO_v3_2_IlmThread__3_5_0',
            '-DILMTHREAD_NAMESPACE_CUSTOM=1'])
        disabled = ['DCMTK', 'FFmpeg', 'Freetype', 'GIF', 'JXL', 'LibRaw', 'Libheif',
                    'Nuke', 'OpenCV', 'OpenGL', 'OpenJPEG', 'OpenVDB', 'Ptex',
                    'QT', 'Qt5', 'Qt6', 'R3DSDK', 'TBB', 'libuhdr', 'openjph', 'PYTHON']
        build('OpenImageIO', [
            '-DBUILD_SHARED_LIBS=ON', '-DEMBEDPLUGINS=ON', '-DUSE_SIMD=sse2',
            '-DOIIO_BUILD_TESTS=OFF', '-DOIIO_BUILD_TOOLS=OFF', '-DUSE_CCACHE=OFF',
            '-DOpenImageIO_BUILD_MISSING_DEPS=none',
            '-DOpenEXR_DIR=' + str(prefix / 'lib/cmake/OpenEXR'),
            '-DOIIO_USE_EXR_C_API=ON', '-DOIIO_USE_HWY=OFF',
            *['-DUSE_' + name + '=OFF' for name in disabled]])
        receipt['installed_sha256'] = inventory(prefix)
        receipt['dependencies_unchanged'] = inventory(deps) == before
        if not receipt['dependencies_unchanged']:
            raise ValueError('Read-only dependency prefix changed')
        for name, cache in caches.items():
            if verify_cache(cache, commits[name]) != trees[name]:
                raise ValueError('Read-only source cache changed')
        receipt['success'] = True
    except Exception as error:
        receipt['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps(dict(prefix=str(prefix), receipt=str(output / 'receipt.json'))))


def inventory_without_git(source):
    return {p.relative_to(source).as_posix(): digest(p)
            for p in sorted(source.rglob('*')) if p.is_file()
            and '.git' not in p.relative_to(source).parts}


if __name__ == '__main__':
    main()
