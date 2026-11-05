"""Bind a tested Windows build to its PE closure, source snapshot and test logs."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from runtime_closure import audit, digest, overlays
from product_validation import execute_for_freeze, load as load_validation

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--ctest-log', type=Path, required=True)
parser.add_argument('--color-overlay', type=Path)
parser.add_argument('--bounded-overlay', type=Path)
parser.add_argument('--product-validation', type=Path, help='Independent no-test product execution report (instead of a full CTest build)')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
build, sdk = args.build.resolve(), args.sdk.resolve()
exe = build / 'Release/thumbgen.exe'
log = args.ctest_log.read_text(encoding='utf-8-sig')
if args.product_validation:
    if not log.strip():
        raise RuntimeError('Independent product build log is empty')
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    if 'BUILD_TESTING:BOOL=OFF' not in cache or re.search(r'^Python3_EXECUTABLE:', cache, re.M):
        raise RuntimeError('Independent product must have tests OFF and no test Python')
    ctest = dict(scope='independent product; historical full CTest not rerun',
                 validation_sha256=digest(args.product_validation), build_log_sha256=digest(args.ctest_log))
else:
    result = re.search(r'100% tests passed, 0 tests failed out of (\d+)', log)
    if not result or int(result[1]) < 116:
        raise RuntimeError('Full Windows test success not present in supplied log')
    suite = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '-C', 'Release', '--show-only=json-v1'], text=True))
    test_names = {test['name'] for test in suite['tests']}
    passed_names = set(re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.{2,}\s+Passed', log))
    if passed_names != test_names or len(test_names) != int(result[1]):
        raise RuntimeError('Supplied test log does not cover the configured suite')
    ctest = dict(passed=int(result[1]), names=sorted(test_names), log_sha256=digest(args.ctest_log))
closure = audit(exe, sdk, Path(os.environ['SystemRoot']) / 'System32', args.color_overlay, args.bounded_overlay)
if closure['sdk_profile'] != 'source':
    raise RuntimeError('Internal runtime candidate requires the minimal source SDK')
if args.product_validation:
    executed_validation = execute_for_freeze(load_validation(args.product_validation), closure,
        args.output.resolve().parent / (args.output.stem + '.execution'))
    ctest['validation_sha256'] = digest(executed_validation)
environment = dict(SystemRoot=os.environ['SystemRoot'], PATH=str(sdk / 'sdk/bin') + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32'))
identity = json.loads(subprocess.check_output([str(exe), '--version', '--json'], env=environment, text=True, timeout=30))
capabilities = json.loads(subprocess.check_output([str(exe), 'capabilities', '--json'], env=environment, text=True, timeout=30))
overlays.require_bounded_selection(capabilities, args.bounded_overlay)
if capabilities['cpu']['oiio_simd'] != 'sse2':
    raise RuntimeError('Candidate does not meet requested OIIO SSE2 profile')
source_files = ['CMakeLists.txt', 'CMakePresets.json']
source_files += [str(path.relative_to(repo)).replace('\\', '/') for directory in ('src', 'include', 'tests', 'schemas', 'cmake', 'scripts', 'dependencies')
                 for path in sorted((repo / directory).rglob('*'))
                 if path.is_file() and path.suffix in ('.cpp', '.h', '.in', '.py', '.json', '.txt', '.patch', '.cmake', '.ps1', '.md')]
build_provenance = dict(cache_sha256=digest(build / 'CMakeCache.txt'),
                        projects={p.name: digest(p) for p in sorted(build.glob('thumbgen*.vcxproj'))})
link = build / 'thumbgen.dir/Release/thumbgen.tlog/link.command.1.tlog'
if args.color_overlay or args.bounded_overlay:
    command = link.read_text(encoding='utf-16').lower().replace('\\', '/')
    expected = []
    if args.color_overlay:
        expected += [args.color_overlay.resolve() / 'lib/lcms2.lib', args.color_overlay.resolve() / 'lib/jpeg.lib']
    if args.bounded_overlay:
        expected.append(args.bounded_overlay.resolve() / 'lib/OpenEXRCore_v_3_5_0_OpenImageIO_v3_2.lib')
    for library in expected:
        if library.as_posix().lower() not in command:
            raise RuntimeError('Selected overlay not present in linker command: ' + str(library))
    build_provenance.update(link_command_sha256=digest(link), link_command_path=str(link),
        selected_libraries={str(p): digest(p) for p in expected}, bounded_core_link=bool(args.bounded_overlay))
report = dict(schema_version=1, kind='thumbgen-tested-windows-product', identity=identity, capabilities=capabilities,
              closure=closure, exe_sha256=digest(exe), ctest=ctest, build_provenance=build_provenance,
              test_executables={path.name: digest(path) for path in sorted((build / 'Release').glob('*.exe'))},
              source_snapshot={relative: digest(repo / relative) for relative in sorted(set(source_files))},
              sdk_receipt_sha256=digest(sdk / 'sdk/thumbgen-sdk.json'),
              source_commit_at_collection=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              limitation='Collected after testing; file binding is local build evidence, not signed build attestation or minimum-platform acceptance.')
report['validation_mode'] = 'independent-v2' if args.product_validation else 'full-ctest'
if args.product_validation:
    report['submitted_validation'] = dict(path=str(args.product_validation.resolve()), sha256=digest(args.product_validation))
    report['product_validation'] = dict(path=str(executed_validation), sha256=digest(executed_validation))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(exe_sha256=report['exe_sha256'], closure_files=len(closure['files']), validation=ctest)))
