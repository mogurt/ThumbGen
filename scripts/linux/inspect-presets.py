"""Audit archive presets, release scope, smoke output, and full-test guard failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zlib
import struct

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('logs', type=Path)
args = parser.parse_args()
source = args.source.resolve()
logs = args.logs.resolve()
logs.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ)
environment.pop('OCIO', None)
release_environment = dict(environment)
release_environment.pop('THUMBGEN_TEST_PYTHON', None)


def command(name, arguments, env, expected=0, message=None):
    result = subprocess.run(list(map(str, arguments)), cwd=source, env=env, capture_output=True, text=True)
    (logs / (name + '.log')).write_text(result.stdout + result.stderr)
    if (result.returncode == 0) != (expected == 0) or (message and message not in result.stdout + result.stderr):
        raise RuntimeError(f'{name}: unexpected configure/run result ({result.returncode})')
    return result


release = source / 'build/linux-release'
cache = (release / 'CMakeCache.txt').read_text()
python_cache = [line for line in cache.splitlines() if re.match(r'_?Python3[^=]*[:=]', line)]
if python_cache:
    raise RuntimeError('Release configured Python lookup/cache entries')
targets = re.findall(r'^build ([^\n]+): CXX_(?:STATIC_LIBRARY|EXECUTABLE)_LINKER',
                     (release / 'build.ninja').read_text(), re.MULTILINE)
if set(targets) != {'thumbgen', 'libthumbgen_core.a', 'libthumbgen_runtime.a', 'libthumbgen_schedule.a'}:
    raise RuntimeError('Release product target set differs: ' + repr(targets))
options = {key: value.upper() for key, value in re.findall(r'^([A-Za-z0-9_]+):BOOL=([^\r\n]*)$', cache, re.MULTILINE)}
if (any(options.get(key) not in ('OFF','FALSE','NO','0') for key in
        ('BUILD_TESTING','THUMBGEN_BUILD_PROBES','THUMBGEN_BUILD_VIPS')) or
        options.get('THUMBGEN_VERIFY_DEPENDENCY_VERSIONS') not in ('ON','TRUE','YES','1')):
    raise RuntimeError('Release preset options differ')
versions = json.loads((source / 'dependencies/versions.json').read_text())
binary = release / 'thumbgen'
version = json.loads(command('release-version', [binary, '--version', '--json'], release_environment).stdout)
if version['build']['revision'] != 'unknown' or (source / '.git').exists():
    raise RuntimeError('Expected archive identity without .git')


def chunk(kind, payload):
    return struct.pack('!I', len(payload)) + kind + payload + struct.pack('!I', zlib.crc32(kind + payload))


image = logs / 'synthetic.png'
image.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 2, 1, 8, 2, 0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(b'\0\xff\0\0\xff\0\0')) + chunk(b'IEND', b''))
target = logs / 'release-output.png'
smoke = command('release-smoke', [binary, 'convert', image, '--output', target, '--json'], release_environment)
rows = [json.loads(line) for line in smoke.stdout.splitlines() if line]
if not rows[0]['ok'] or not target.read_bytes().startswith(b'\x89PNG'):
    raise RuntimeError('Release PNG smoke failed')
command('negative-no-python', ['cmake', '--preset', 'linux-dev', '-B', source / 'build/negative-no-python'],
        release_environment, expected=1, message='Full tests require BUILD_TESTING=ON')
command('negative-no-protocol', ['cmake', '--preset', 'linux-dev', '-B', source / 'build/negative-no-protocol',
                                '-DTHUMBGEN_TEST_PROTOCOL=OFF'], environment, expected=1,
        message='Full tests require THUMBGEN_TEST_PROTOCOL=ON')
dev_cache = (source / 'build/linux-dev/CMakeCache.txt').read_text()
json_directory = Path(re.search(r'^nlohmann_json_DIR:PATH=(.+)$', dev_cache, re.MULTILINE)[1])
fake = source / 'build/negative-json-package'
fake.mkdir()
(fake / 'nlohmann_jsonConfigVersion.cmake').write_bytes((json_directory / 'nlohmann_jsonConfigVersion.cmake').read_bytes())
(fake / 'nlohmann_jsonConfig.cmake').write_text(f'include("{json_directory}/nlohmann_jsonConfig.cmake")\nset(nlohmann_json_VERSION "3.11.99")\n')
command('negative-json-version', ['cmake', '--preset', 'linux-release', '-B', source / 'build/negative-json-version',
                                 '-Dnlohmann_json_DIR=' + str(fake)], release_environment, expected=1,
        message='Locked SDK versions required')
report = dict(source=str(source), archive_without_git=True, revision=version['build']['revision'],
              release_product_targets=targets, release_python_cache_entries=python_cache,
              dependency_versions_required=versions, release_version=version, release_png_success=True,
              release_binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              negative_guards=dict(no_python=True, no_protocol=True, json_version_drift=True))
(logs / 'presets-inspection.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
