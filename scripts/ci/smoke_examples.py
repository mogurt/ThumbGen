"""Run unchanged public manifests in a fresh private layout and read all outputs.

Requires a completed dev/release build and its SDK runtime environment. This uses
only repository synthetic generators; it does not download assets or build SDKs.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from jsonschema import Draft202012Validator


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit_rows(manifest, rows, validator):
    for row in rows:
        validator.validate(row)
    results = [row for row in rows if row['type'] == 'result']
    summaries = [row for row in rows if row['type'] == 'summary']
    jobs = {job['id']: job for job in manifest['jobs']}
    if len(jobs) != len(manifest['jobs']) or Counter(row['id'] for row in results) != Counter(jobs.keys()):
        raise RuntimeError('Public example result IDs differ from manifest')
    if len(summaries) != 1 or rows[-1] != summaries[0]:
        raise RuntimeError('Public example requires one final summary')
    output_count = sum(len(job['outputs']) for job in jobs.values())
    summary = summaries[0]
    if (summary['total'] != len(jobs) or summary['success'] != len(jobs) or
        any(summary[key] for key in ('failed', 'partial', 'skipped', 'cancelled')) or
        summary['output_counts']['total'] != output_count or
        summary['output_counts']['success'] != output_count or
        any(value for key, value in summary['output_counts'].items() if key not in ('total', 'success'))):
        raise RuntimeError('Public example summary arithmetic differs')
    for row in results:
        if not row['ok'] or row['status'] != 'success' or len(row['outputs']) != len(jobs[row['id']]['outputs']):
            raise RuntimeError('Public example did not produce all requested outputs')
        if any(output['status'] != 'success' for output in row['outputs']):
            raise RuntimeError('Public example output failed or skipped')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'linux'), required=True)
    parser.add_argument('--parallel', type=int, default=2)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    logs = repo / 'build/ci' / args.platform
    sandbox = logs / 'public-smoke'
    sandbox.mkdir(parents=True, exist_ok=False)
    commands = []
    report = dict(status='running', commands=commands, manifests={})
    def save():
        (sandbox / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    def run(name, command):
        command = list(map(str, command))
        record = dict(name=name, argv=command)
        commands.append(record)
        save()
        # All fixture compiler outputs live here, never under the SDK prefix.
        with (sandbox / (name + '.log')).open('w', encoding='utf-8') as log:
            result = subprocess.run(command, cwd=sandbox, stdout=log, stderr=subprocess.STDOUT,
                                    text=True, encoding='utf-8', timeout=180)
        record['returncode'] = result.returncode
        save()
        if result.returncode:
            raise RuntimeError(name + ' failed; see ' + str(sandbox / (name + '.log')))
        return (sandbox / (name + '.log')).read_text(encoding='utf-8')
    try:
        build = sandbox / 'fixture-build'
        command = ['cmake', '-S', repo / 'scripts/ci/fixtures', '-B', build,
                   '-DCMAKE_PREFIX_PATH=' + os.environ['THUMBGEN_SDK_PREFIX']]
        command += ['-G', 'Visual Studio 17 2022', '-A', 'x64'] if args.platform == 'windows' else ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release']
        run('configure-fixtures', command)
        run('build-fixtures', ['cmake', '--build', build, '--config', 'Release', '--parallel', args.parallel])
        def exe(directory, name):
            return directory / ('Release/' + name + '.exe' if args.platform == 'windows' else name)
        deps = sandbox / '.deps'
        deps.mkdir()
        run('colour-fixtures', [exe(build, 'public_fixtures'), deps / 'public-inputs'])
        dev = repo / 'build' / (args.platform + '-dev')
        run('aov-fixtures', [exe(dev, 'thumbgen_aov_tests'), '--fixtures', deps / 'public-aov'])
        run('crypto-fixtures', [exe(dev, 'thumbgen_cryptomatte_tests'), '--fixtures', deps / 'public-crypto'])
        before = {str(path.relative_to(deps)): sha256(path) for path in deps.rglob('*') if path.is_file()}
        examples = sandbox / 'examples/public'
        examples.mkdir(parents=True)
        validator = Draft202012Validator(json.loads((repo / 'schemas/protocol-v1.schema.json').read_text(encoding='utf-8')))
        binary = exe(repo / 'build' / (args.platform + '-release'), 'thumbgen')
        for source in sorted((repo / 'examples/public').glob('*.jobs.json')):
            manifest_path = examples / source.name
            shutil.copyfile(source, manifest_path)
            manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
            raw = run(source.stem, [binary, 'batch', '--manifest', manifest_path, '--json'])
            rows = [json.loads(line) for line in raw.splitlines() if line.strip()]
            results = audit_rows(manifest, rows, validator)
            outputs = {}
            for row in results:
                job = next(job for job in manifest['jobs'] if job['id'] == row['id'])
                expected_paths = {(manifest_path.parent / output['path']).resolve() for output in job['outputs']}
                if {Path(output['path']).resolve() for output in row['outputs']} != expected_paths:
                    raise RuntimeError('Public example output paths differ from manifest')
                for index, output in enumerate(row['outputs']):
                    path = Path(output['path'])
                    run(source.stem + '-' + row['id'] + '-' + str(index),
                        [exe(build, 'read_preview'), path, output['width'], output['height']])
                    outputs[str(path.relative_to(sandbox))] = sha256(path)
            if source.name == 'sequence.jobs.json':
                if [row['selection']['lock_status'] for row in results] != ['created', 'reused']:
                    raise RuntimeError('Sequence example did not create/reuse its selection lock')
                if len(set(outputs.values())) != 1:
                    raise RuntimeError('Repeated sequence input produced different bytes')
            report['manifests'][source.name] = dict(sha256=sha256(source), jobs=len(results), outputs=outputs)
        if not report['manifests']:
            raise RuntimeError('No public examples executed')
        after = {str(path.relative_to(deps)): sha256(path) for path in deps.rglob('*') if path.is_file()}
        if before != after:
            raise RuntimeError('Public example inputs changed')
        report.update(status='passed', inputs_sha256=before)
    except BaseException as error:
        report.update(status='failed', failure=str(error))
        raise
    finally:
        save()
    print(json.dumps(report['manifests'], indent=2))


if __name__ == '__main__':
    main()
