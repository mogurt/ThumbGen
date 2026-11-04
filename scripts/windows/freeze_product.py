"""Bind a tested Windows build to its PE closure, source snapshot and test logs."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from runtime_closure import audit, digest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--ctest-log', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
build, sdk = args.build.resolve(), args.sdk.resolve()
exe = build / 'Release/thumbgen.exe'
log = args.ctest_log.read_text(encoding='utf-8-sig')
result = re.search(r'100% tests passed, 0 tests failed out of (\d+)', log)
if not result or int(result[1]) < 116:
    raise RuntimeError('Full Windows test success not present in supplied log')
suite = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '-C', 'Release', '--show-only=json-v1'], text=True))
test_names = {test['name'] for test in suite['tests']}
passed_names = set(re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.{2,}\s+Passed', log))
if passed_names != test_names or len(test_names) != int(result[1]):
    raise RuntimeError('Supplied test log does not cover the configured suite')
closure = audit(exe, sdk, Path(os.environ['SystemRoot']) / 'System32')
if closure['sdk_profile'] != 'source':
    raise RuntimeError('Internal runtime candidate requires the minimal source SDK')
environment = dict(os.environ, PATH=str(sdk / 'sdk/bin') + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32'), OCIO='')
identity = json.loads(subprocess.check_output([str(exe), '--version', '--json'], env=environment, text=True, timeout=30))
capabilities = json.loads(subprocess.check_output([str(exe), 'capabilities', '--json'], env=environment, text=True, timeout=30))
if capabilities['cpu']['oiio_simd'] != 'sse2':
    raise RuntimeError('Candidate does not meet requested OIIO SSE2 profile')
source_files = ['CMakeLists.txt', 'CMakePresets.json', 'dependencies/versions.json']
source_files += [str(path.relative_to(repo)).replace('\\', '/') for directory in ('src', 'include', 'tests', 'schemas')
                 for path in sorted((repo / directory).rglob('*'))
                 if path.is_file() and path.suffix in ('.cpp', '.h', '.in', '.py', '.json', '.txt')]
report = dict(schema_version=1, kind='thumbgen-tested-windows-product', identity=identity, capabilities=capabilities,
              closure=closure, exe_sha256=digest(exe), ctest=dict(passed=int(result[1]), names=sorted(test_names), log_sha256=digest(args.ctest_log)),
              test_executables={path.name: digest(path) for path in sorted((build / 'Release').glob('*.exe'))},
              source_snapshot={relative: digest(repo / relative) for relative in sorted(set(source_files))},
              sdk_receipt_sha256=digest(sdk / 'sdk/thumbgen-sdk.json'),
              source_commit_at_collection=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              limitation='Collected after testing; file binding is local build evidence, not signed build attestation or minimum-platform acceptance.')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(exe_sha256=report['exe_sha256'], closure_files=len(closure['files']), tests=int(result[1]))))
