"""Stage test artifacts and non-glibc runtime closure; this is not a release packager."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import re

cache = Path(sys.argv[1]).resolve()
repo = Path(__file__).resolve().parents[2]
stage = Path(os.environ.get('THUMBGEN_VALIDATION_DIR', repo / '.deps/linux/validation')).resolve()
stage.mkdir(parents=True, exist_ok=True)
for directory in ('bin', 'lib', 'tests', 'schemas', 'tools'):
    (stage / directory).mkdir(exist_ok=True)
binaries = ('thumbgen', 'thumbgen_core_tests', 'thumbgen_image_matrix', 'thumbgen_budget_tests', 'thumbgen_process_tests', 'thumbgen_control_tests', 'thumbgen_worker_protocol_tests', 'thumbgen_isolated_tests')
for name in binaries:
    shutil.copy2(cache / 'thumbgen-build' / name, stage / 'bin' / name)
logs = Path(os.environ.get('THUMBGEN_BUILD_LOG_DIR', cache / 'logs'))
audit = json.loads((logs / 'abi.json').read_text())
if audit['failures']:
    raise SystemExit('ABI audit failed; refusing staging')
# glibc components must always come from the target OS.
system = ('libc.so', 'libm.so', 'libpthread.so', 'libdl.so', 'librt.so',
          'libresolv.so', 'libutil.so', 'ld-linux-', 'libnss_', 'libanl.so',
          'libcrypt.so', 'libnsl.so', 'libBrokenLocale.so', 'libthread_db.so')
copied = []
for obj in audit['objects']:
    path = Path(obj['path'])
    dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    soname = re.search(r'\(SONAME\).*?\[(.*?)\]', dynamic)
    identity = soname[1] if soname else path.name
    if path.name in binaries or identity.startswith(system) or path.name.startswith('ld-'):
        continue
    destination = stage / 'lib' / path.name
    shutil.copy2(path, destination)
    if soname and soname[1] != path.name:
        alias = stage / 'lib' / soname[1]
        # Use copies on the shared Windows filesystem so both WSL distros see them.
        shutil.copy2(path, alias)
    copied.append(str(path))
for name in ('test_cli.py', 'test_protocol.py', 'test_interrupt.py', 'requirements.txt'):
    shutil.copy2(repo / 'tests' / name, stage / 'tests' / name)
for path in (repo / 'schemas').glob('*.json'):
    shutil.copy2(path, stage / 'schemas' / path.name)
# Discover the authoritative suite from configured CTest, rather than maintaining
# a second image-case list that can silently fall behind CMakeLists.txt.
ctest = cache / 'tools/cmake-3.31.8-linux-x86_64/bin/ctest'
suite = json.loads(subprocess.check_output(
    [str(ctest), '--test-dir', str(cache / 'thumbgen-build'), '--show-only=json-v1'], text=True))
replacements = {str(cache / 'thumbgen-build' / name): '@BIN@/' + name for name in binaries}
replacements.update({str(repo / 'tests/test_cli.py'): '@TESTS@/test_cli.py',
                     str(repo / 'tests/test_protocol.py'): '@TESTS@/test_protocol.py',
                     str(repo / 'tests/test_interrupt.py'): '@TESTS@/test_interrupt.py',
                     str(repo / 'schemas'): '@SCHEMAS@',
                     str(cache / 'sdk/lib'): '@LIB@',
                     str(cache / 'tools/python-3.11.11/bin/python3'): '@PYTHON@'})
tests = []
for test in suite['tests']:
    command = []
    for argument in test['command']:
        value = replacements.get(argument, argument)
        if value.startswith('/'):
            raise SystemExit('Unmapped build-host test argument: ' + value)
        command.append(value)
    properties = {}
    for prop in test.get('properties', []):
        if prop['name'] in ('LABELS', 'TIMEOUT', 'SKIP_RETURN_CODE'):
            properties[prop['name']] = prop['value']
        elif prop['name'] == 'ENVIRONMENT_MODIFICATION':
            supported = {f'{key}=path_list_prepend:{cache / "sdk/lib"}'
                         for key in ('PATH', 'LD_LIBRARY_PATH')}
            if not set(prop['value']).issubset(supported):
                raise SystemExit('Unmapped test environment: ' + test['name'])
        elif prop['name'] == 'WORKING_DIRECTORY':
            if prop['value'] != str(cache / 'thumbgen-build'):
                raise SystemExit('Unmapped test working directory: ' + test['name'])
        else:
            raise SystemExit('Unmapped test property: ' + prop['name'])
    tests.append(dict(name=test['name'], command=command, properties=properties))
if not tests or not any(test['command'][0] == '@BIN@/thumbgen_image_matrix' for test in tests):
    raise SystemExit('Configured image matrix is missing')
if not any(test['name'] == 'public_protocol' for test in tests):
    raise SystemExit('Configured protocol test is missing; refusing incomplete validation')
if not any(test['name'] == 'public_interrupt' for test in tests):
    raise SystemExit('Configured interrupt test is missing; refusing incomplete validation')
(stage / 'tests/ctest-suite.json').write_text(json.dumps(dict(tests=tests), indent=2) + '\n')
(stage / 'tools/cmake/bin').mkdir(parents=True, exist_ok=True)
shutil.copy2(ctest, stage / 'tools/cmake/bin/ctest')
# Strip build-host paths from staged ELF copies using CMake's built-in editor.
cmake = cache / 'tools/cmake-3.31.8-linux-x86_64/bin/cmake'
commands = []
for path in sorted(list((stage / 'bin').iterdir()) + list((stage / 'lib').iterdir())):
    dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    old = re.search(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', dynamic)
    if old:
        replacement = '$ORIGIN/../lib' if path.parent.name == 'bin' else '$ORIGIN'
        commands.append(f'file(RPATH_CHANGE FILE "{path}" OLD_RPATH "{old[1]}" NEW_RPATH "{replacement}")')
rpath_script = cache / 'stage-rpath.cmake'
rpath_script.write_text('\n'.join(commands) + '\n')
subprocess.check_call([str(cmake), '-P', str(rpath_script)])
(stage / 'runtime-provenance.json').write_text(json.dumps(dict(copied=copied, audit=audit), indent=2))
print(stage)
