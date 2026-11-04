"""Record actual source identities, build flags and staged artifact hashes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

cache = Path(sys.argv[1]).resolve()
repo = Path(os.environ.get('THUMBGEN_SOURCE_DIR', Path(__file__).resolve().parents[2])).resolve()
build = Path(os.environ.get('THUMBGEN_PRODUCT_BUILD_DIR', cache / 'thumbgen-build')).resolve()
expected = json.loads((repo / 'scripts/linux/sources-lock.json').read_text(encoding='utf-8-sig'))
def run(*args):
    return subprocess.check_output(args, text=True).strip()
records = []
for dependency in expected['dependencies']:
    source = cache / 'oiio-deps' / dependency['package']
    actual = run('git', '-C', str(source), 'rev-parse', 'HEAD')
    if actual != dependency['commit']:
        raise SystemExit('Source mismatch: ' + dependency['package'])
    records.append(dict(dependency, actual_commit=actual))
flags = {}
for root in (cache / 'oiio-deps', cache / 'oiio-build', build):
    for path in root.rglob('CMakeCache.txt'):
        values = [line for line in path.read_text(errors='replace').splitlines()
                  if line.startswith(('CMAKE_C_FLAGS:', 'CMAKE_CXX_FLAGS:', 'USE_SIMD:'))]
        if any('-march=native' in value or '-mtune=native' in value for value in values):
            raise SystemExit('Host CPU flags detected: ' + str(path))
        flags[str(path)] = values
stage = Path(os.environ.get('THUMBGEN_VALIDATION_DIR', repo / '.deps/linux/validation')).resolve()
build_identity = json.loads(run(str(stage / 'bin/thumbgen'), '--version', '--json'))['build']
artifacts = {}
for directory in ('bin', 'lib'):
    for path in sorted((stage / directory).iterdir()):
        artifacts[str(path.relative_to(stage))] = hashlib.sha256(path.read_bytes()).hexdigest()
archive_receipt = os.environ.get('THUMBGEN_ARCHIVE_IDENTITY')
if archive_receipt:
    archive = json.loads(Path(archive_receipt).read_text())
    source_hashes = {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in archive['sha256']}
    if source_hashes != archive['sha256'] or (repo / '.git').exists() or build_identity['revision'] != 'unknown':
        raise SystemExit('Archive source/build identity mismatch')
    product_source = dict(commit=archive['commit'], worktree_commit=None, sha256=source_hashes,
                          archive_sha256=archive['archive_sha256'])
else:
    product_source = dict(commit=run('git', '-C', str(repo), 'rev-parse', build_identity['revision']),
                          worktree_commit=run('git', '-C', str(repo), 'rev-parse', 'HEAD'),
                          sha256={str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in [repo / 'CMakeLists.txt', *sorted((repo / 'src').glob('*.cpp')),
                                               *sorted((repo / 'src').glob('*.h*')), *sorted((repo / 'include').rglob('*.h')),
                                               *sorted((repo / 'tests').glob('*.cpp')), *sorted((repo / 'tests').glob('*.h')),
                                               *sorted((repo / 'tests').glob('*.py')), repo / 'tests/requirements.txt',
                                               *sorted((repo / 'schemas').glob('*.json'))]})
report = dict(source_dependencies=records, flags=flags, staged_artifacts=artifacts,
              test_dependencies=json.loads((repo / 'scripts/linux/test-deps-lock.json').read_text()),
              ctest_suite=json.loads((stage / 'tests/ctest-suite.json').read_text()),
              build_identity=build_identity,
              product_source=product_source,
              tools={name: run(*command) for name, command in {
                  'gcc': ['g++', '-dumpfullversion'], 'git': ['git', '--version'],
                  'cmake': ['cmake', '--version'], 'ninja': ['ninja', '--version'],
                  'python': ['python3', '--version'], 'glibc': ['getconf', 'GNU_LIBC_VERSION']}.items()},
              rpms=run('rpm', '-qa').splitlines())
(stage / 'build-evidence.json').write_text(json.dumps(report, indent=2) + '\n')
print('Verified source commits:', len(records), 'CMake flag sets:', len(flags), 'artifacts:', len(artifacts))
