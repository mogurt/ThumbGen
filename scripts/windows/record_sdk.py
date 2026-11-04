"""Record a source-built SDK's installed file contents and pinned build inputs."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache', type=Path, required=True)
parser.add_argument('--request', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
cache = args.cache.resolve()
lock_file = repo / 'dependencies/windows-source-sdk.json'
lock = json.loads(lock_file.read_text(encoding='utf-8-sig'))
expected = dict([(item['package'], item['commit']) for item in lock['dependencies']] +
                [('OpenImageIO', lock['oiio_commit']), ('nlohmann_json', lock['nlohmann_json']['commit'])])
sources = json.loads((cache / 'source-receipt.json').read_text())
if {item['package']: item['commit'] for item in sources} != expected:
    raise RuntimeError('SDK source receipts differ from the lock')
for item in sources:
    source = cache / item['destination']
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != item['commit']:
        raise RuntimeError('SDK source changed: ' + item['package'])
    # Preserve helper-generated source changes, rather than asserting Git HEAD
    # alone proves all compiled bytes are the original source.
    patch = subprocess.check_output(['git', '-C', str(source), 'diff', '--binary'])
    patch_file = args.request.parent / (item['package'] + '.patch')
    patch_file.write_bytes(patch)
    item['source_patch_sha256'] = hashlib.sha256(patch).hexdigest()
    item['source_patch_nonempty'] = bool(patch)
artifacts = {}
for directory in ('sdk', 'oiio-deps/dist'):
    root = cache / directory
    if not root.is_dir():
        raise RuntimeError('Missing SDK install directory: ' + directory)
    for path in sorted(root.rglob('*')):
        if path.is_file() and path.name != 'thumbgen-sdk.json':
            if not path.resolve().is_relative_to(root.resolve()):
                raise RuntimeError('SDK artifact escapes install prefix: ' + str(path))
            artifacts[path.relative_to(cache).as_posix()] = dict(sha256=digest(path), size=path.stat().st_size)
if not any(name.lower().endswith('/openimageio.dll') for name in artifacts):
    raise RuntimeError('Installed OIIO DLL missing')
request = json.loads(args.request.read_text(encoding='utf-8-sig'))
owners = {}
# Dependency manifests precede OIIO's manifest, which also installs copies of
# those libraries. Match content hashes so the copies retain their true owner.
ordered_sources = sorted(sources, key=lambda item: item['package'] == 'OpenImageIO')
for item in ordered_sources:
    build = ('oiio-build' if item['package'] == 'OpenImageIO' else 'json-build'
             if item['package'] == 'nlohmann_json' else 'oiio-deps/' + item['package'] + '-build')
    manifest = cache / build / 'install_manifest.txt'
    if not manifest.is_file():
        raise RuntimeError('Missing component install manifest: ' + item['package'])
    for line in manifest.read_text().splitlines():
        path = Path(line)
        if path.suffix.lower() != '.dll':
            continue
        relative = path.resolve().relative_to(cache).as_posix()
        if relative not in artifacts:
            raise RuntimeError('Installed DLL missing from receipt: ' + relative)
        owners.setdefault(artifacts[relative]['sha256'], item['package'])
redist = next(value.split('=', 1)[1] for value in request['arguments'] if value.startswith('-DMSVC_REDIST_DIR='))
runtime_artifacts = []
for name, artifact in artifacts.items():
    if not name.lower().endswith('.dll'):
        continue
    basename = Path(name).name
    if re.match(r'(?:msvcp|vcruntime|concrt)\d', basename, re.IGNORECASE):
        original = Path(redist) / 'x64/Microsoft.VC143.CRT' / basename
        if not original.is_file() or digest(original) != artifact['sha256']:
            raise RuntimeError('VC runtime differs from selected VS redistributable: ' + basename)
        artifact['component'] = 'MSVC-runtime'
        runtime_artifacts.append(name)
    else:
        artifact['component'] = owners[artifact['sha256']]
jpeg = (cache / 'oiio-deps/libjpeg-turbo-build/CMakeCache.txt').read_text()
if 'WITH_SIMD:BOOL=ON' not in jpeg:
    raise RuntimeError('JPEG SIMD disabled; not an accepted performance SDK')
flags = (cache / 'oiio-build/build.ninja').read_text()
if not all(token in flags for token in ('/O2', '/EHsc', '/D__SSE2__')):
    raise RuntimeError('OIIO optimization, exception or SSE2 flags missing')
if '/D__SSE4_1__' in flags or '/D__SSE4_2__' in flags or '/arch:AVX' in flags:
    raise RuntimeError('OIIO flags exceed requested generic x64/SSE2 profile')
if digest(cache / 'sdk/include/OpenImageIO/simd.h') != digest(cache / 'oiio-source/src/include/OpenImageIO/simd.h'):
    raise RuntimeError('Installed SIMD header differs from patched source')
scripts = {name: digest(repo / 'scripts/windows' / name) for name in
           ('build-sdk.ps1', 'prepare_sdk_sources.py', 'sdk_patches.py', 'record_sdk.py')}
nasm = Path(next(value.split('=', 1)[1] for value in request['arguments'] if value.startswith('-DCMAKE_ASM_NASM_COMPILER=')))
report = dict(schema_version=1, kind='thumbgen-windows-source-sdk', source_lock=lock,
              source_lock_sha256=digest(lock_file), sources=sources,
              request=request, runtime=dict(component='MSVC-runtime', source=redist, artifacts=runtime_artifacts),
              build_scripts_sha256=scripts, nasm_sha256=digest(nasm),
              jpeg_simd=True, artifacts=artifacts, redistribution_ready=False,
              limitations=['Recorded installed file hashes are build evidence, not a trusted signature.',
                           'Static/embedded notices and minimum target platform acceptance remain separate gates.'])
target = cache / 'sdk/thumbgen-sdk.json'
target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(artifacts=len(artifacts), sources=len(sources), jpeg_simd=True)))
