"""Run public dev/release presets and audit complete CTest/release scope."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback
import xml.etree.ElementTree as ET


def audit_tests(inventory, junit):
    names = [test['name'] for test in inventory['tests']]
    required = {'public_cli', 'public_protocol', 'public_interrupt', 'public_aov',
                'public_cryptomatte', 'public_sampled_id', 'public_deep', 'core_deep_flatten', 'core_deep_reader', 'core_deep_output', 'core_deep_selection', 'core_deep_runtime', 'core_deep_infinite'}
    if not required.issubset(names) or len(set(names)) != len(names):
        raise RuntimeError('Incomplete or duplicate CTest registration')
    disabled = [test['name'] for test in inventory['tests']
                for prop in test.get('properties', [])
                if prop['name'] == 'DISABLED' and str(prop['value']).upper() not in
                ('FALSE', 'OFF', '0', 'NO', '', 'NONE', 'NOTFOUND')]
    if disabled:
        raise RuntimeError('Disabled CTest registration: ' + repr(disabled))
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
        if actual != expected or len([t for t in targets if t['type'] != 'UTILITY']) != len(expected):
            raise RuntimeError('Release target scope differs: ' + repr(actual))
    return expected


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_identity(repo):
    """Also work in a source archive, where Git metadata is deliberately absent."""
    if not (repo / '.git').exists():
        return dict(kind='source-directory-without-git')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args], text=True, encoding='utf-8').strip()
    return dict(kind='git', commit=git('rev-parse', 'HEAD'), status=git('status', '--porcelain'))


class Commands:
    """Keep the failing command and partial output even when process launch fails."""
    def __init__(self, repo, logs, env, report):
        self.repo, self.logs, self.env, self.report = repo, logs, env, report

    def save(self):
        (self.logs / 'execution.json').write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')

    def __call__(self, name, command, environment=None, failure=None, quiet=False):
        command = list(map(str, command))
        print('+', ' '.join(command), flush=True)
        record = dict(name=name, argv=command, cwd=str(self.repo), started=time.time(),
                      log=name + '.log', expected_failure=failure)
        self.report['commands'].append(record)
        self.save()
        lines = []
        try:
            with (self.logs / record['log']).open('w', encoding='utf-8') as log:
                with subprocess.Popen(command, cwd=self.repo, env=environment if environment is not None else self.env,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, encoding='utf-8', errors='replace') as process:
                    for line in process.stdout:
                        log.write(line)
                        log.flush()
                        lines.append(line)
                        if not quiet:
                            print(line, end='', flush=True)
                    record['returncode'] = process.wait()
        except BaseException as error:
            record['error'] = str(error)
            raise
        finally:
            record['finished'] = time.time()
            self.save()
        output = ''.join(lines)
        if failure is None:
            if record['returncode']:
                raise RuntimeError(f'{name} failed ({record["returncode"]}); log retained')
        elif record['returncode'] == 0 or failure not in output:
            raise RuntimeError(name + ': expected explicit full-test guard rejection')
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'linux'), required=True)
    parser.add_argument('--parallel', type=int, default=2, help='Maximum product/fixture build parallelism (default: 2)')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    logs = repo / 'build/ci' / args.platform
    logs.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    report = dict(platform=args.platform, status='running', commands=[],
                  sdk_prefix=env.get('THUMBGEN_SDK_PREFIX'),
                  conda_archive_cache=env.get('CONDA_PKGS_DIRS'),
                  test_python=env.get('THUMBGEN_TEST_PYTHON'), driver_sha256=digest(Path(__file__)))
    run = Commands(repo, logs, env, report)
    run.save()
    try:
        report['source'] = source_identity(repo)
        report['input_sha256'] = {path.relative_to(repo).as_posix(): digest(path)
                                  for path in (repo / 'CMakePresets.json', repo / 'scripts/linux/sources-lock.json',
                                               repo / 'dependencies/windows-sdk.explicit.txt')}
        if args.parallel < 1:
            raise RuntimeError('--parallel must be positive')
        execute(args, repo, logs, env, run)
        report['status'] = 'passed'
    except BaseException:
        report['status'] = 'failed'
        report['failure'] = traceback.format_exc()
        raise
    finally:
        run.save()


def execute(args, repo, logs, env, run):
    dev = args.platform + '-dev'
    release = args.platform + '-release'
    # A repeated run must choose a fresh worktree; stale results cannot pass CI.
    if any((repo / 'build' / preset).exists() for preset in (dev, release)):
        raise RuntimeError('CI requires fresh dev and release build directories')
    python = env.get('THUMBGEN_TEST_PYTHON')
    if not python:
        raise RuntimeError('Explicit THUMBGEN_TEST_PYTHON is required')

    run('tools', ['cmake', '--version'])
    run('ctest-version', ['ctest', '--version'])
    run('python-version', [python, '--version'])
    run('ci-audits', [python, '-m', 'unittest', 'discover', '-s', 'tests/ci', '-v'])
    run('test-python', [python, '-c', 'import jsonschema; print(jsonschema.__file__)'])
    run('public-examples', [python, 'scripts/public/validate_examples.py'])
    run('public-example-checks', [python, '-m', 'unittest', 'discover', '-s', 'tests/public', '-v'])
    run('packaging-checks', [python, '-m', 'unittest', 'discover', '-s', 'tests/packaging', '-v'])
    run('configure-dev', ['cmake', '--preset', dev])
    run('build-dev', ['cmake', '--build', '--preset', dev, '--parallel', args.parallel])
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
    run('build-release', ['cmake', '--build', '--preset', release, '--parallel', args.parallel], release_env)
    binary = repo / 'build' / release / ('Release/thumbgen.exe' if args.platform == 'windows' else 'thumbgen')
    runtime_env = dict(release_env)
    if args.platform == 'windows':
        runtime_env['PATH'] = os.pathsep.join(str(Path(p) / 'bin') for p in env['THUMBGEN_SDK_PREFIX'].split(';')) + os.pathsep + env['PATH']
    run('release-version', [binary, '--version', '--json'], runtime_env)
    run('public-example-runtime', [python, 'scripts/ci/smoke_examples.py', '--platform', args.platform,
                                  '--parallel', args.parallel], runtime_env)
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
                  public_example_runtime_checked=True,
                  release_targets=targets, release_test_python=False,
                  full_test_guards=['no-python', 'no-protocol', 'no-jsonschema'])
    result['sha256'] = {path.relative_to(repo).as_posix(): digest(path)
                        for path in (binary, junit, logs / 'ctest-inventory.json',
                                     logs / 'public-smoke/report.json')}
    (logs / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
