"""Attest fresh install roots/build logs and reject old SDK library contamination."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('cache', type=Path)
parser.add_argument('old_cache', type=Path)
args = parser.parse_args()
cache = args.cache.resolve()
old = args.old_cache.resolve()
if cache == old or old in cache.parents:
    raise RuntimeError('Expected independent cache root outside the existing cache')
lock = json.loads((cache / 'source-archive/scripts/linux/sources-lock.json').read_text())
for directory in ('oiio-build', 'oiio-deps/dist', 'sdk', 'json-build'):
    if not (cache / directory).is_dir():
        raise RuntimeError('Missing fresh build/install directory: ' + directory)
forbidden = [str(old / name) for name in ('sdk', 'oiio-deps/dist', 'oiio-build', 'json-build', 'thumbgen-build')]
caches = {}
package_locations = {}
non_installing_cache_defaults = []
install_roots = {'oiio-build/CMakeCache.txt', 'json-build/CMakeCache.txt'}
install_roots.update('oiio-deps/' + item['package'] + '-build/CMakeCache.txt' for item in lock['dependencies'])
for path in cache.rglob('CMakeCache.txt'):
    lines = path.read_text(errors='replace').splitlines()
    used = [line for line in lines if not line.startswith(('CMAKE_IGNORE_', '//', '#')) and
            any(prefix in line for prefix in forbidden)]
    if used or any('-march=native' in line or '-mtune=native' in line for line in lines):
        raise RuntimeError('Forbidden old SDK path or host CPU flag: ' + str(path) + repr(used))
    prefix = [line.split('=', 1)[1] for line in lines if line.startswith('CMAKE_INSTALL_PREFIX:PATH=')]
    if prefix and not Path(prefix[0]).resolve().is_relative_to(cache):
        if path.relative_to(cache).as_posix() in install_roots:
            raise RuntimeError('Install prefix escapes independent cache: ' + str(path))
        non_installing_cache_defaults.append(str(path.relative_to(cache)))
    caches[str(path.relative_to(cache))] = hashlib.sha256(path.read_bytes()).hexdigest()
    packages = {}
    for line in lines:
        if line.startswith(('OpenImageIO_DIR:PATH=', 'nlohmann_json_DIR:PATH=')) and not line.endswith('-NOTFOUND'):
            name, value = line.split('=', 1)
            if not Path(value).resolve().is_relative_to(cache):
                raise RuntimeError('Product package lookup escaped new cache: ' + line)
            packages[name.split(':', 1)[0]] = value
    if packages:
        package_locations[str(path.relative_to(cache))] = packages
dependency_logs = {}
link_files = {}
for filename in ('build.ninja', 'link.txt'):
    for path in cache.rglob(filename):
        lines = path.read_text(errors='replace').splitlines()
        if any(any(prefix in line for prefix in forbidden) and 'CMAKE_IGNORE_PREFIX_PATH' not in line for line in lines):
            raise RuntimeError('Old SDK path in generated build/link instructions: ' + str(path))
        link_files[str(path.relative_to(cache))] = hashlib.sha256(path.read_bytes()).hexdigest()
for item in lock['dependencies']:
    path = cache / 'oiio-deps' / (item['package'] + '-build.log')
    text = path.read_text()
    if '==== configure (exit code 0)' not in text or '==== build (exit code 0)' not in text or '==== install (exit code 0)' not in text:
        raise RuntimeError('Dependency build/install not proven: ' + item['package'])
    dependency_logs[item['package']] = dict(version=item['version'], commit=item['commit'], log=str(path.relative_to(cache)),
                                           sha256=hashlib.sha256(path.read_bytes()).hexdigest())
installed = {}
install_manifests = {}
for manifest in cache.rglob('install_manifest*.txt'):
    if not (manifest.parent / 'cmake_install.cmake').is_file():
        continue  # A dependency source tree may contain CI/example manifests.
    entries = manifest.read_text().splitlines()
    if any(not Path(entry).resolve().is_relative_to(cache) for entry in entries if entry):
        raise RuntimeError('Installed path escapes independent cache: ' + str(manifest))
    install_manifests[str(manifest.relative_to(cache))] = hashlib.sha256(manifest.read_bytes()).hexdigest()
for directory in (cache / 'sdk', cache / 'oiio-deps/dist'):
    for path in directory.rglob('*'):
        if path.is_symlink() and not path.resolve().is_relative_to(cache):
            raise RuntimeError('Install symlink escapes new cache: ' + str(path))
        if path.is_file():
            installed[str(path.relative_to(cache))] = hashlib.sha256(path.read_bytes()).hexdigest()
receipt = dict(cache=str(cache), previous_cache=str(old),
               tools_reused=True, tool_environment=str(old / 'environment.sh'), installed_sdk_artifacts_reused=False,
               tool_environment_sha256=hashlib.sha256((old / 'environment.sh').read_bytes()).hexdigest(),
               source_clones=json.loads((cache / 'source-seed.json').read_text()),
               dependency_builds=dependency_logs, cmake_caches=caches, installed_sha256=installed,
               install_manifests=install_manifests, non_installing_cache_defaults=non_installing_cache_defaults,
               generated_build_link_files=link_files,
               package_locations=package_locations,
               limits='Existing compiler/toolchain, OS glibc/C++ runtime and test Python reused; not a clean-machine rebuild')
(cache / 'sdk-rebuild.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(dict(dependency_builds=len(dependency_logs), installed_files=len(installed), cmake_caches=len(caches)), indent=2))
