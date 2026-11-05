"""Run public dev/release presets and audit complete CTest/release scope."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def audit_tests(inventory, junit):
    names = [test['name'] for test in inventory['tests']]
    required = {'public_cli', 'public_protocol', 'public_interrupt', 'public_aov',
                'public_cryptomatte', 'public_sampled_id', 'core_deep_flatten', 'core_deep_reader'}
    if not required.issubset(names) or len(set(names)) != len(names):
        raise RuntimeError('Incomplete or duplicate CTest registration')
    cases = ET.fromstring(junit).findall('.//testcase')
    if Counter(case.attrib['name'] for case in cases) != Counter(names):
        raise RuntimeError('JUnit does not match the entire registered CTest inventory')
    bad = [case.attrib['name'] for case in cases if
           case.find('failure') is not None or case.find('error') is not None or
           case.find('skipped') is not None or case.attrib.get('status') in ('notrun', 'disabled')]
    if bad:
        raise RuntimeError('Failed, skipped or disabled tests: ' + repr(bad))
    return len(names)


def audit_release(directory):
    cache = (directory / 'CMakeCache.txt').read_text(encoding='utf-8')
    if re.search(r'^_?Python\w*:', cache, re.MULTILINE | re.IGNORECASE):
        raise RuntimeError('Release performed test Python discovery')
    for name in ('BUILD_TESTING', 'THUMBGEN_BUILD_PROBES', 'THUMBGEN_BUILD_VIPS',
                 'THUMBGEN_BUILD_STRESS_FIXTURES'):
        if not re.search(r'^' + name + r':BOOL=(OFF|FALSE|0)\s*$', cache, re.MULTILINE):
            raise RuntimeError('Unexpected release option: ' + name)
    reply = directory / '.cmake/api/v1/reply'
    index = json.loads(max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime).read_text())
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
    if not model['configurations']:
        raise RuntimeError('Release codemodel has no configurations')
    expected = {'thumbgen': 'EXECUTABLE', 'thumbgen_core': 'STATIC_LIBRARY',
                'thumbgen_runtime': 'STATIC_LIBRARY', 'thumbgen_schedule': 'STATIC_LIBRARY'}
    for config in model['configurations']:
        targets = [json.loads((reply / item['jsonFile']).read_text()) for item in config['targets']]
        actual = {target['name']: target['type'] for target in targets if target['type'] != 'UTILITY'}
        if actual != expected:
            raise RuntimeError('Release target scope differs: ' + repr(actual))
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'linux'), required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    logs = repo / 'build/ci' / args.platform
    logs.mkdir(parents=True, exist_ok=False)
    dev = args.platform + '-dev'
    release = args.platform + '-release'
    # A repeated run must choose a fresh worktree; stale results cannot pass CI.
    if any((repo / 'build' / preset).exists() for preset in (dev, release)):
        raise RuntimeError('CI requires fresh dev and release build directories')
    env = dict(os.environ)
    python = env.get('THUMBGEN_TEST_PYTHON')
    if not python:
        raise RuntimeError('Explicit THUMBGEN_TEST_PYTHON is required')

    def run(name, command, environment=env, failure=None, quiet=False):
        print('+', ' '.join(map(str, command)), flush=True)
        lines = []
        # Persist output as it arrives, including before a runner cancellation.
        with (logs / (name + '.log')).open('w', encoding='utf-8') as log:
            with subprocess.Popen(list(map(str, command)), cwd=repo, env=environment,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding='utf-8', errors='replace') as process:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    lines.append(line)
                    if not quiet:
                        print(line, end='', flush=True)
                returncode = process.wait()
        output = ''.join(lines)
        if failure is None:
            if returncode:
                raise RuntimeError(f'{name} failed ({returncode}); log retained')
        elif returncode == 0 or failure not in output:
            raise RuntimeError(name + ': expected explicit full-test guard rejection')
        return output

    run('test-python', [python, '-c', 'import jsonschema; print(jsonschema.__file__)'])
    run('public-examples', [python, 'scripts/public/validate_examples.py'])
    run('public-example-checks', [python, '-m', 'unittest', 'discover', '-s', 'tests/public', '-v'])
    run('packaging-checks', [python, '-m', 'unittest', 'discover', '-s', 'tests/packaging', '-v'])
    run('configure-dev', ['cmake', '--preset', dev])
    run('build-dev', ['cmake', '--build', '--preset', dev])
    inventory = run('ctest-inventory', ['ctest', '--preset', dev, '--show-only=json-v1'], quiet=True)
    (logs / 'ctest-inventory.json').write_text(inventory, encoding='utf-8')
    test_env = dict(env, OCIO='intentionally-invalid-parent-config.ocio')
    junit = repo / 'build' / dev / 'ci-tests.xml'
    run('ctest', ['ctest', '--preset', dev, '--output-junit', junit], test_env)
    count = audit_tests(json.loads(inventory), junit.read_text(encoding='utf-8'))
    print(f'Audited all {count} registered tests against JUnit', flush=True)
    release_env = dict(env)
    release_env.pop('THUMBGEN_TEST_PYTHON', None)
    query = repo / 'build' / release / '.cmake/api/v1/query/codemodel-v2'
    query.parent.mkdir(parents=True)
    query.touch()
    run('configure-release', ['cmake', '--preset', release], release_env)
    targets = audit_release(repo / 'build' / release)
    run('build-release', ['cmake', '--build', '--preset', release], release_env)
    binary = repo / 'build' / release / ('Release/thumbgen.exe' if args.platform == 'windows' else 'thumbgen')
    runtime_env = dict(release_env)
    if args.platform == 'windows':
        runtime_env['PATH'] = os.pathsep.join(str(Path(p) / 'bin') for p in env['THUMBGEN_SDK_PREFIX'].split(';')) + os.pathsep + env['PATH']
    run('release-version', [binary, '--version', '--json'], runtime_env)
    run('negative-no-python', ['cmake', '--preset', dev, '-B', logs / 'negative-no-python'],
        release_env, failure='Full tests require BUILD_TESTING=ON')
    run('negative-no-protocol', ['cmake', '--preset', dev, '-B', logs / 'negative-no-protocol',
                                '-DTHUMBGEN_TEST_PROTOCOL=OFF'], failure='Full tests require THUMBGEN_TEST_PROTOCOL=ON')
    empty = logs / 'python-without-dependencies'
    run('empty-test-python', [sys.executable, '-m', 'venv', '--without-pip', empty])
    empty_python = empty / ('Scripts/python.exe' if args.platform == 'windows' else 'bin/python')
    run('negative-no-jsonschema', ['cmake', '--preset', dev, '-B', logs / 'negative-no-jsonschema',
                                 '-DPython3_EXECUTABLE=' + str(empty_python)],
        failure='Protocol tests require tests/requirements.txt')
    result = dict(platform=args.platform, registered_tests=count, executed_tests=count,
                  public_examples_checked=True,
                  packaging_checks_checked=True,
                  release_targets=targets, release_test_python=False,
                  full_test_guards=['no-python', 'no-protocol', 'no-jsonschema'])
    (logs / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
