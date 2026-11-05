"""Verify a complete local execution report against the selected PE closure.

This checks evidence consistency and coverage, not cryptographic execution.
The validator and this verifier must be clean tracked contents of the trusted
checkout; producer paths are evidence locations, not validator identities.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys

REPO = Path(__file__).resolve().parents[2]
VERSION = 2
FILES = ('tests/packaging/validate_overlay_product.py', 'scripts/windows/product_validation.py',
         'schemas/protocol-v1.schema.json')
CASES = {'capabilities': None, 'rgb.png': (8, 8, 2), 'gray.png': (8, 8, 2),
         'cmyk.tif': (8, 8, 2), 'cmyk.jpg': (8, 8, 2), 'ycck.jpg': (8, 8, 2),
         'ordinary-rgb': (8, 8, 1), 'deep-scan': (129, 9, 1), 'deep-tile': (129, 9, 1)}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate validation key: ' + key)
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique)


def validator_identity():
    result = {}
    for relative in FILES:
        path = REPO / relative
        committed = subprocess.check_output(['git', '-C', str(REPO), 'show', 'HEAD:' + relative])
        data = path.read_bytes()
        if data.replace(b'\r\n', b'\n') != committed.replace(b'\r\n', b'\n'):
            raise ValueError('Untrusted modified validation recipe: ' + relative)
        result[relative] = dict(path=str(path), sha256=sha(path),
            lf_sha256=hashlib.sha256(committed.replace(b'\r\n', b'\n')).hexdigest())
    return result


def check_report(report, closure):
    if (type(report.get('schema_version')) is not int or report.get('schema_version') != VERSION or report.get('kind') != 'independent-windows-product-validation'
            or report.get('passed') is not True):
        raise ValueError('Expected complete independent validation v2 report')
    deep = any(item['kind'] == 'bounded' for item in closure.get('overlays', []))
    required_cases = {name: layout for name, layout in CASES.items() if deep or not name.startswith('deep-')}
    artifacts = {}
    for entry in report['artifacts']:
        path = Path(entry['path'])
        key = str(path.resolve()).casefold()
        if not path.is_absolute() or key in artifacts or sha(path) != entry['sha256']:
            raise ValueError('Invalid/duplicate/changed validation artifact: ' + str(path))
        artifacts[key] = entry['sha256']

    def bound(entry):
        path = Path(entry['path'])
        if artifacts.get(str(path.resolve()).casefold()) != entry['sha256']:
            raise ValueError('Missing validation artifact coverage: ' + str(path))
        return path

    trusted = validator_identity()
    if set(report['validator']) != set(trusted):
        raise ValueError('Validation recipe identity set differs')
    for relative, current in trusted.items():
        recorded = report['validator'][relative]
        path = bound(recorded)
        if (recorded['lf_sha256'] != current['lf_sha256'] or
                hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != current['lf_sha256']):
            raise ValueError('Validation recipe content differs: ' + relative)
    reference = Path(report['fixtures']['color_evidence']) / 'report.json'
    bound(dict(path=str(reference), sha256=sha(reference)))
    expected = {entry['name'].casefold(): entry['sha256'] for entry in closure['files']}
    runtime = report['tested_runtime']
    if set(runtime) != set(expected) or report['exe_sha256'] != closure['exe_sha256']:
        raise ValueError('Tested executable/runtime differs from frozen closure')
    exe = Path(report['tested_exe']).resolve()
    for name, entry in runtime.items():
        path = bound(entry).resolve()
        if path.parent != exe.parent or path.name.casefold() != name or entry['sha256'] != expected[name]:
            raise ValueError('Tested runtime path/digest differs: ' + name)
    if runtime.get(exe.name.casefold(), {}).get('path') != str(exe):
        raise ValueError('Tested EXE path differs')
    modules = report['loaded_modules']
    if load(bound(report['module_index'])) != modules:
        raise ValueError('Loaded module index differs')
    windows = Path(os.environ.get('SystemRoot', 'C:/Windows')).resolve()
    observed = set()
    for text, checksum in modules.items():
        path = Path(text).resolve(); name = path.name.casefold()
        if sha(path) != checksum:
            raise ValueError('Loaded module digest differs: ' + text)
        if name in runtime:
            if path != Path(runtime[name]['path']).resolve() or checksum != expected[name]:
                raise ValueError('Loaded runtime substituted: ' + text)
            observed.add(name)
        elif not (path.is_relative_to(windows / 'System32') or path.is_relative_to(windows / 'WinSxS')):
            raise ValueError('Unexpected loaded module: ' + text)
    if observed != set(expected):
        raise ValueError('Missing loaded runtime coverage: ' + str(set(expected) - observed))
    cases = report['cases']
    if len(cases) != len(required_cases) or {case['name'] for case in cases} != set(required_cases):
        raise ValueError('Required validation case coverage differs')
    if load(bound(report['command_index'])) != cases:
        raise ValueError('Validation command index differs')
    for case in cases:
        if case['exit'] != 0 or Path(case['command'][0]).resolve() != exe:
            raise ValueError('Validation command failed or used another EXE')
        stdout = bound(case['stdout']); bound(case['stderr'])
        rows = [json.loads(line) for line in stdout.read_bytes().splitlines() if line.strip()]
        layout = required_cases[case['name']]
        if layout is None:
            if len(rows) != 1 or rows[0].get('type') != 'capabilities':
                raise ValueError('Missing capabilities output')
            if ('zstd' in rows[0].get('deep_compressions', [])) != deep:
                raise ValueError('Validation capability differs from selected bounded Core')
            continue
        target = bound(case['output']).resolve()
        if ('--output' not in case['command'] or
                Path(case['command'][case['command'].index('--output') + 1]).resolve() != target):
            raise ValueError('Validation output command binding differs')
        results = [row for row in rows if row.get('type') == 'result']
        if len(results) != 1 or not results[0].get('ok') or not any(
                entry.get('status') == 'success' and Path(entry['path']).resolve() == target
                for entry in results[0].get('outputs', [])):
            raise ValueError('Missing successful validation output log')
        from PIL import Image
        with Image.open(target) as image:
            image.load()
            if image.size != layout[:2]:
                raise ValueError('Validation output dimensions differ')
        if case['pixels'] != layout[0] * layout[1] or not 0 <= case['max_lsb'] <= layout[2]:
            raise ValueError('Validation pixel coverage/result differs')
    return dict(schema_version=VERSION, cases=len(cases), runtime_files=len(runtime),
                pixels=sum(layout[0] * layout[1] for layout in required_cases.values() if layout))


def check_frozen(evidence, closure, require_independent=True):
    mode = evidence.get('validation_mode')
    if mode == 'independent-v2':
        entry = evidence['product_validation']
        if sha(entry['path']) != entry['sha256']:
            raise ValueError('Frozen independent validation report changed')
        return check_report(load(entry['path']), closure)
    if mode == 'full-ctest' and not require_independent:
        tests = evidence['ctest']
        if tests['passed'] < 116 or len(set(tests['names'])) != tests['passed']:
            raise ValueError('Incomplete full CTest evidence')
        return None
    raise ValueError('Missing/unknown frozen validation mode; re-freeze with the trusted tools')


def execute_for_freeze(submitted, closure, destination):
    """Run trusted code on a new exact closure copy; never import a success claim."""
    check_report(submitted, closure)
    validator_identity()
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    binary = destination / 'bin'; binary.mkdir()
    for entry in closure['files']:
        source = Path(entry['source_path'])
        if sha(source) != entry['sha256']:
            raise ValueError('Runtime changed before independent execution')
        shutil.copyfile(source, binary / entry['name'])
    # The known AMD64 CLI name is also part of the audited report, not a caller's
    # alternate executable or arbitrary command/validator path.
    exe = binary / Path(submitted['tested_exe']).name
    command = [sys.executable, str(REPO / FILES[0]), '--exe', str(exe),
        '--color-evidence', submitted['fixtures']['color_evidence'],
        '--deep-evidence', submitted['fixtures']['deep_evidence'], '--output', str(destination / 'results')]
    with (destination / 'validator.stdout').open('xb') as stdout, (destination / 'validator.stderr').open('xb') as stderr:
        result = subprocess.run(command, cwd=destination, stdout=stdout, stderr=stderr, timeout=180)
    (destination / 'invocation.json').write_text(json.dumps(dict(command=command, exit=result.returncode), indent=2), encoding='utf-8')
    if result.returncode:
        raise ValueError('Trusted independent validator execution failed; original logs retained in ' + str(destination))
    report = destination / 'results/report.json'
    check_report(load(report), closure)
    return report
