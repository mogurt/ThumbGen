"""Freeze a new product-only install plus its audited Linux runtime closure.

Build with the archived linux-release preset, --parallel 2, and
-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib first. Never edits an SDK or source tree.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from runtime_overlays import audit, audit_base, cache_values, require, run, sha, system_elf

p = argparse.ArgumentParser(description=__doc__)
for name in ('source', 'archive', 'build', 'install', 'cache', 'color-root', 'core-root', 'output', 'cmake'):
    p.add_argument('--' + name, required=True, type=Path)
p.add_argument('--dwa-root', type=Path, help='Optional portable-DWA build root containing prefix, source and native build trees')
a = p.parse_args()
require(not a.output.exists(), 'Stage must be a new private directory')
source, build, install, stage = (x.resolve() for x in (a.source, a.build, a.install, a.output))
with tarfile.open(a.archive) as archive:
    commit = archive.pax_headers.get('comment', '').strip()
    require(re.fullmatch('[a-f0-9]{40}', commit), 'Expected Windows Git archive commit')
    mapping = {m.name: hashlib.sha256(archive.extractfile(m).read()).hexdigest() for m in archive if m.isfile()}
require(not (source / '.git').exists() and all(sha(source / n) == h for n, h in mapping.items()), 'Frozen archive tree changed')
values = cache_values(build)
require(values['BUILD_TESTING'] == 'OFF' and values['CMAKE_BUILD_TYPE'] == 'Release', 'Expected product-only Release')
require(values['CMAKE_HOME_DIRECTORY'] == str(source), 'Product build source differs')
require(values['CMAKE_CXX_FLAGS'] == '-march=x86-64 -mtune=generic', 'Product CPU flags differ')
require(values['CMAKE_INSTALL_PREFIX'] == str(install), 'Install prefix differs')
require(not any(k.startswith('Python') for k in values), 'Test Python entered product configure')
commands = run('ninja', '-C', build, '-t', 'commands', 'thumbgen')
base = audit_base(source, a.cache)
overlay = audit(source, a.color_root, a.core_root, build, a.dwa_root, base)
stage.mkdir(parents=True)
for name in ('bin', 'lib'):
    (stage / name).mkdir()
shutil.copy2(install / 'bin/thumbgen', stage / 'bin/thumbgen')
require(str(install / 'bin/thumbgen') in (build / 'install_manifest.txt').read_text().splitlines(), 'Product not in install manifest')
env = dict(PATH=os.environ['PATH'], HOME=os.environ['HOME'], LANG='C', LC_ALL='C',
           LD_LIBRARY_PATH=':'.join(str(x) for x in [a.color_root / 'sdk/lib64', a.color_root / 'sdk/lib',
                *([a.dwa_root / 'prefix/lib'] if a.dwa_root else []), a.cache / 'sdk/lib', a.cache / 'oiio-deps/dist/lib']))
loader = subprocess.check_output(['ldd', str(install / 'bin/thumbgen')], env=env, text=True)
require('not found' not in loader, 'Unresolved installation closure')
expected_shared = dict(base['verified_files'])
expected_shared.update(overlay['verified_files'])
objects, copied = [], []
for item in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader):
    path = Path(item).resolve()
    if not path.is_file():
        continue
    dynamic = run('readelf', '-d', path)
    match = re.search(r'\(SONAME\).*?\[(.*?)\]', dynamic)
    soname = match[1] if match else path.name
    objects.append(dict(path=str(path), sha256=sha(path)))
    if system_elf(path):
        continue
    allowed = expected_shared.get(str(path))
    if soname in ('libstdc++.so.6', 'libgcc_s.so.1'):
        package = run('rpm', '-qf', path)
        require(package.startswith(('libstdc++-4.8.5-', 'libgcc-4.8.5-')), 'Unknown compiler runtime')
        dump = run('rpm', '-q', '--dump', package)
        row = next((s.split() for s in dump.splitlines() if Path(s.split()[0]).resolve() == path and s.split()[3] != '0' * 64), None)
        require(row and len(row[3]) == 64 and sha(path) == row[3], 'RPM runtime content differs')
        allowed = sha(path)
        objects[-1]['rpm'] = package
    require(allowed == sha(path), 'Unknown or changed runtime library: ' + str(path))
    shutil.copy2(path, stage / 'lib' / path.name)
    if soname != path.name:
        (stage / 'lib' / soname).symlink_to(path.name)
    copied.append(dict(source=str(path), file='lib/' + path.name, sha256=sha(path)))
edits = []
for path in sorted((stage / 'lib').iterdir()):
    if path.is_symlink():
        continue
    dynamic = run('readelf', '-d', path)
    match = re.search(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', dynamic)
    if match:
        edits.append(f'file(RPATH_CHANGE FILE "{path}" OLD_RPATH "{match[1]}" NEW_RPATH "$ORIGIN")')
edit = stage / 'stage-rpath.cmake'
edit.write_text('\n'.join(edits) + '\n')
run(a.cmake, '-P', edit)
identity = json.loads(run(stage / 'bin/thumbgen', '--version', '--json'))
require(identity['build']['revision'] == 'unknown', 'Archive build should have unknown embedded Git revision')
caps = json.loads(run(stage / 'bin/thumbgen', 'capabilities', '--json'))
require('zstd' in caps['deep_compressions'], 'Bounded ZSTD capability inconsistent with selected Core')
receipt = dict(commit=commit, archive_sha256=sha(a.archive), sha256=mapping)
(stage / 'archive-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
artifacts = {x.relative_to(stage).as_posix(): sha(x) for folder in ('bin', 'lib') for x in sorted((stage / folder).iterdir()) if x.is_file()}
evidence = dict(source_dependencies=json.loads((source / 'scripts/linux/sources-lock.json').read_text())['dependencies'],
    staged_artifacts=artifacts, build_identity=identity['build'], product_source=dict(commit=commit, sha256=mapping),
    product_install=dict(executable_sha256=sha(install / 'bin/thumbgen'), build_executable_sha256=sha(build / 'thumbgen'),
        configure=values, commands=commands, files={str(x): sha(x) for x in
            [build / 'build.ninja', build / 'compile_commands.json', build / 'install_manifest.txt', build / 'CMakeCache.txt',
             *sorted(p for p in build.parent.glob('*.log') if p.name.startswith(('configure', 'build', 'install')))]}),
    overlays=overlay, original_sdk=base, copied=copied, loader=loader,
    tools={n: run(*cmd) for n, cmd in {'gcc':['g++','--version'], 'cmake':[a.cmake,'--version'],
        'ninja':['ninja','--version'], 'glibc':['getconf','GNU_LIBC_VERSION'], 'readelf':['readelf','--version']}.items()})
(stage / 'build-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
(stage / 'runtime-provenance.json').write_text(json.dumps(dict(audit=dict(objects=objects)), indent=2) + '\n')
print(json.dumps(dict(stage=str(stage), artifacts=len(artifacts), source_commit=commit)))
