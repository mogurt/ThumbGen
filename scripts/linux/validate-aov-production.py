"""Run a frozen Windows AOV workload and a same-OS frozen Linux colour baseline."""
import argparse
from collections import Counter
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import signal
import subprocess

from jsonschema import Draft202012Validator


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def linux_path(path):
    value = PureWindowsPath(path)
    if value.drive:
        return Path('/mnt') / value.drive[0].lower() / Path(*value.parts[1:])
    return Path(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--baseline-stage', type=Path, required=True)
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    args.directory.mkdir(exist_ok=False)
    reference = json.loads((args.references / 'windows-mixed-report.json').read_text())
    manifest = json.loads((args.references / 'windows-mixed-jobs.json').read_text())
    originals = reference['input_sha256']
    assert len(originals) == 20 and len(manifest['jobs']) == 48
    assert set(originals) == {job['input'] for job in manifest['jobs']}
    original_repro = json.loads((args.references / 'windows-repro-v1-report.json').read_text())['input_sha256']
    assert len(original_repro) == 1 and set(original_repro.values()).issubset(set(originals.values()))
    inputs = args.directory / 'inputs'
    inputs.mkdir()
    mapped = {}
    source_receipt = []
    for index, (path, expected) in enumerate(originals.items()):
        original = linux_path(path)
        if digest(original) != expected:
            raise ValueError('Original source differs from frozen Windows SHA: ' + path)
        target = inputs / (str(index) + '-' + original.name)
        shutil.copyfile(original, target)
        if digest(target) != expected or digest(original) != expected:
            raise ValueError('Source changed while copying: ' + path)
        target.chmod(0o444)
        mapped[path] = str(target)
        source_receipt.append(dict(original=str(original), windows_path=path, copied=str(target), sha256=expected))
    inputs.chmod(0o555)
    (args.directory / 'source-inputs.json').write_text(json.dumps(source_receipt, indent=2) + '\n')
    schema = Draft202012Validator(json.loads((args.source / 'schemas/protocol-v1.schema.json').read_text()))
    jobs_schema = Draft202012Validator(json.loads((args.source / 'schemas/jobs-v1.schema.json').read_text()))
    module = importlib.util.spec_from_file_location('encoding', Path(__file__).with_name('verify-preview-encoding.py'))
    encoding = importlib.util.module_from_spec(module)
    module.loader.exec_module(encoding)
    colour_jobs = [job for job in manifest['jobs'] if job['id'].startswith('colour-')]
    assert len(colour_jobs) == 20
    report = dict(archive_source=str(args.source), source_inputs=source_receipt,
                  references_sha256={path.name: digest(path) for path in args.references.iterdir() if path.is_file()},
                  helper_sha256=digest(args.helper), baseline_exe_sha256=digest(args.baseline_stage / 'bin/thumbgen'),
                  new_exe_sha256=digest(args.stage / 'bin/thumbgen'), runs=[])
    baseline = {}
    data_hashes = {}
    mismatches = []
    for name, stage, selected in (('d095-colour-baseline', args.baseline_stage, colour_jobs), ('aov-mixed', args.stage, manifest['jobs'])):
        directory = args.directory / name
        directory.mkdir()
        request = copy.deepcopy(manifest)
        request['jobs'] = copy.deepcopy(selected)
        for job in request['jobs']:
            job['input'] = mapped[job['input']]
            for output in job['outputs']:
                output['path'] = str(directory / PureWindowsPath(output['path']).name)
        jobs_schema.validate(request)
        jobs_path = directory / 'jobs.json'
        jobs_path.write_text(json.dumps(request, indent=2) + '\n')
        env = dict(os.environ, LD_LIBRARY_PATH=str(stage / 'lib'), OCIO='ocio://default')
        process = subprocess.Popen([str(stage / 'bin/thumbgen'), 'batch', '--manifest', str(jobs_path), '--json'],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        timeout = False
        try:
            stdout, stderr = process.communicate(timeout=600)
        except subprocess.TimeoutExpired:
            timeout = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
        (directory / 'stdout.jsonl').write_text(stdout)
        (directory / 'stderr.txt').write_text(stderr)
        process_record = dict(exit=process.returncode, timeout=timeout)
        (directory / 'process.json').write_text(json.dumps(process_record, indent=2) + '\n')
        assert process.returncode == 0 and not timeout, process_record
        records = [json.loads(line) for line in stdout.splitlines() if line.strip()]
        assert len(records) == len(selected) + 1 and records[-1]['success'] == len(selected)
        checks = []
        for row in records:
            schema.validate(row)
            if row['type'] != 'result':
                continue
            assert row['status'] == 'success'
            if not row['id'].startswith('colour-'):
                assert row['colour']['config_basis'] == 'none'
            for output in row['outputs']:
                code = encoding.inspect_encoding(output['path'])
                read = subprocess.run([str(args.helper), output['path']], env=env, text=True, capture_output=True)
                pixels = json.loads(read.stdout)
                assert read.returncode == 0 and pixels['readable']
                assert (pixels['width'], pixels['height']) == (output['width'], output['height'])
                checks.append(dict(id=row['id'], encoding=code, pixels=pixels))
                key = (row['id'], output['format'], output['width'], output['height'])
                if name == 'd095-colour-baseline':
                    baseline[key] = code['sha256']
                elif row['id'].startswith('colour-'):
                    if code['sha256'] != baseline[key]:
                        mismatches.append(dict(key=key, old=baseline[key], new=code['sha256']))
                else:
                    data_hashes[key] = code['sha256']
        item = dict(name=name, exe_sha256=digest(stage / 'bin/thumbgen'), records=records, process=process_record,
                    checks=checks, encoding_counts=dict(Counter(check['encoding']['format'] for check in checks)))
        report['runs'].append(item)
        report['colour_sha_mismatches'] = mismatches
        (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert len(baseline) == 120 and len(data_hashes) == 168
    for (identity, *rest), sha in data_hashes.items():
        if identity.startswith('r2-'):
            assert data_hashes[('r1-' + identity[3:], *rest)] == sha
    assert all(digest(item['original']) == digest(item['copied']) == item['sha256'] for item in source_receipt)
    report.update(tasks=48, new_previews=288, data_views=14, data_previews=168, data_repeats_sha_equal=True,
                  colour_previews=120, colour_previews_equal_to_same_os_d095=not mismatches,
                  baseline_previews_generated=120, source_sha256_unchanged=True, schema_records=49,
                  full_schema_records_including_baseline=70, actual_readback_and_encoding_checks=408)
    (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if mismatches:
        raise AssertionError('Colour output SHA differs from same-OS frozen d095 baseline; see retained report')
    print(json.dumps({key: report[key] for key in ('tasks', 'new_previews', 'data_views', 'data_previews',
          'colour_previews_equal_to_same_os_d095', 'source_sha256_unchanged', 'actual_readback_and_encoding_checks')}))


if __name__ == '__main__':
    main()
