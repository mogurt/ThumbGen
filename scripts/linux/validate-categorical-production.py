"""Compare a new categorical CLI to frozen same-OS AOV and colour outputs, without rerunning old CLIs."""
import argparse
from collections import Counter
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess

from jsonschema import Draft202012Validator


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def previous_references(previous, colour_reference=None):
    """Normalize explicitly supported report generations, without rewriting old evidence."""
    if {'runs', 'source_inputs', 'new_exe_sha256'}.issubset(previous):
        return previous['source_inputs'], previous['new_exe_sha256'], previous['runs']
    if {'checks', 'records', 'sources', 'exe_sha256'}.issubset(previous):
        if colour_reference is None:
            raise ValueError('A checks-shaped previous report requires --colour-reference-report')
        utility = module('aov_reference_utility', 'validate-aov-production.py')
        ancestor = previous
        seen = set()
        while 'runs' not in ancestor:
            if len(seen) >= 16 or not {'previous_report','previous_report_sha256'}.issubset(ancestor):
                raise ValueError('Colour reference chain is incomplete or too deep')
            path = Path(ancestor['previous_report'])
            expected = ancestor['previous_report_sha256']
            if expected in seen or utility.digest(path) != expected:
                raise ValueError('Colour reference chain checksum/cycle failure')
            seen.add(expected)
            ancestor = json.loads(path.read_text())
        if utility.digest(colour_reference) != expected:
            raise ValueError('Colour reference is not the bound ancestor of the previous report')
        baseline = next(run for run in ancestor['runs'] if run['name'] == 'd095-colour-baseline')
        return previous['sources'], previous['exe_sha256'], [baseline, dict(name='aov-mixed', checks=previous['checks'])]
    raise ValueError('Unsupported previous production report structure')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'stage', 'previous-stage', 'previous-production', 'references', 'helper', 'directory'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--colour-reference-report', type=Path, help='Bound older runs-shaped d095 colour baseline for checks-shaped previous reports')
    args = parser.parse_args()
    utility = module('aov_utility', 'validate-aov-production.py')
    encoding = module('preview_encoding', 'verify-preview-encoding.py')
    digest = utility.digest
    original_report = args.previous_production / 'report.json'
    assert digest(original_report) == digest(args.previous_stage / 'results' / os.environ['WSL_DISTRO_NAME'] / 'production-report.json')
    previous = json.loads(original_report.read_text())
    previous_sources, previous_exe, previous_runs = previous_references(previous, args.colour_reference_report)
    assert digest(args.previous_stage / 'bin/thumbgen') == previous_exe
    reference = json.loads((args.references / 'windows-mixed-report.json').read_text())
    request = json.loads((args.references / 'windows-mixed-jobs.json').read_text())
    expected_sources = reference['input_sha256']
    assert len(expected_sources) == 20 and len(request['jobs']) == 48
    assert expected_sources == {row['windows_path']: row['sha256'] for row in previous_sources}
    repro = json.loads((args.references / 'windows-repro-v1-report.json').read_text())['input_sha256']
    assert len(repro) == 1 and set(repro.values()).issubset(set(expected_sources.values()))
    expected = {}; colours = {}
    for run in previous_runs:
        target = expected if run['name'] == 'aov-mixed' else colours
        for check in run['checks']:
            code, pixels = check['encoding'], check['pixels']
            assert digest(code['path']) == code['sha256']
            target[(check['id'], code['format'], pixels['width'], pixels['height'])] = code['sha256']
    assert len(expected) == 288 and len(colours) == 120
    args.directory.mkdir(exist_ok=False)
    inputs = args.directory / 'inputs'; inputs.mkdir()
    sources = []; mapped = {}
    for index, (original, sha) in enumerate(expected_sources.items()):
        source = utility.linux_path(original)
        if digest(source) != sha:
            raise ValueError('Source differs from frozen Windows hash: ' + original)
        target = inputs / (str(index) + '-' + source.name)
        shutil.copyfile(source, target)
        assert digest(target) == digest(source) == sha
        target.chmod(0o444)
        mapped[original] = str(target)
        sources.append(dict(windows_path=original, original=str(source), copied=str(target), sha256=sha))
    inputs.chmod(0o555)
    request = copy.deepcopy(request)
    for job in request['jobs']:
        job['input'] = mapped[job['input']]
        for output in job['outputs']:
            output['path'] = str(args.directory / utility.PureWindowsPath(output['path']).name)
    schema = Draft202012Validator(json.loads((args.source / 'schemas/protocol-v1.schema.json').read_text()))
    Draft202012Validator(json.loads((args.source / 'schemas/jobs-v1.schema.json').read_text())).validate(request)
    jobs = args.directory / 'jobs.json'; jobs.write_text(json.dumps(request, indent=2) + '\n')
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.stage / 'lib'), OCIO='ocio://default')
    process = subprocess.Popen([str(args.stage / 'bin/thumbgen'), 'batch', '--manifest', str(jobs), '--json'],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        timed_out = True; os.killpg(process.pid, signal.SIGKILL); stdout, stderr = process.communicate()
    (args.directory / 'stdout.jsonl').write_text(stdout); (args.directory / 'stderr.txt').write_text(stderr)
    status = dict(exit=process.returncode, timeout=timed_out)
    (args.directory / 'process.json').write_text(json.dumps(status, indent=2) + '\n')
    assert process.returncode == 0 and not timed_out
    records = [json.loads(line) for line in stdout.splitlines() if line.strip()]
    assert len(records) == 49 and records[-1]['success'] == 48
    checks = []; mismatches = []; data = {}; colour_count = 0
    for record in records:
        schema.validate(record)
        if record['type'] != 'result': continue
        assert record['status'] == 'success'
        for output in record['outputs']:
            code = encoding.inspect_encoding(output['path'])
            read = subprocess.run([str(args.helper), output['path']], env=env, capture_output=True, text=True)
            pixels = json.loads(read.stdout)
            assert read.returncode == 0 and pixels['readable']
            assert (pixels['width'], pixels['height']) == (output['width'], output['height'])
            key = (record['id'], code['format'], pixels['width'], pixels['height'])
            if code['sha256'] != expected[key]: mismatches.append(dict(key=key, before=expected[key], after=code['sha256']))
            if record['id'].startswith('colour-'):
                assert code['sha256'] == colours[key], key
                colour_count += 1
            else:
                assert record['colour']['config_basis'] == 'none'
                data[key] = code['sha256']
            checks.append(dict(id=record['id'], encoding=code, pixels=pixels))
    repeat_pairs = 0
    for (identity, *rest), sha in data.items():
        if identity.startswith('r2-'):
            assert data[('r1-' + identity[3:], *rest)] == sha
            repeat_pairs += 1
    assert len(checks) == 288 and colour_count == 120 and len(data) == 168 and repeat_pairs == 84
    assert all(digest(row['original']) == digest(row['copied']) == row['sha256'] for row in sources)
    report = dict(exe_sha256=digest(args.stage / 'bin/thumbgen'), previous_exe_sha256=digest(args.previous_stage / 'bin/thumbgen'),
                  helper_sha256=digest(args.helper), previous_report=str(original_report), previous_report_sha256=digest(original_report),
                  sources=sources, tasks=48, previews=288, schema_records=49, records=records, checks=checks, process=status,
                  compared_previous_output_sha=288, mismatches=mismatches, colour_d095_sha_equal=colour_count,
                  repeated_data_pairs=repeat_pairs, source_sha256_unchanged=True, actual_readbacks=288,
                  encoding_counts=dict(Counter(check['encoding']['format'] for check in checks)),
                  old_cli_rerun=False, new_colour_baseline_generated=False)
    if args.colour_reference_report:
        report['colour_reference_report'] = str(args.colour_reference_report)
        report['colour_reference_report_sha256'] = digest(args.colour_reference_report)
    (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert not mismatches, 'New/previous categorical bytes differ; retained report contains details'
    print(json.dumps({key: report[key] for key in ('tasks', 'previews', 'compared_previous_output_sha', 'colour_d095_sha_equal', 'repeated_data_pairs')}))


if __name__ == '__main__': main()
