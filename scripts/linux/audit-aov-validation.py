"""Audit one OS of a frozen full AOV stage and its production/colour comparison."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from jsonschema import Draft202012Validator


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--production', type=Path, required=True)
    parser.add_argument('--baseline-stage', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, help='Recheck installed SDK identity on build OS')
    parser.add_argument('--audit-output', type=Path, help='Reaudit an identical collected report into a new output file')
    args = parser.parse_args()
    stage = args.stage.resolve()
    repo = stage.parents[2]
    label = os.environ['WSL_DISTRO_NAME']
    directory = stage / 'results' / label
    load = lambda path: json.loads(path.read_text())
    archive = load(stage / 'archive-source.json')
    assert digest(stage / 'source.tar') == archive['archive_sha256']
    for name, expected in archive['sha256'].items():
        assert digest(args.source / name) == expected, name
    assert not (args.source / '.git').exists()
    protected = load(stage / 'preserved-evidence.json')
    for name, expected in protected.items():
        assert digest(repo / name) == expected, name
    for line in (stage / 'PRODUCT-FROZEN.sha256').read_text().splitlines():
        expected, path = line.split(maxsplit=1)
        assert digest(path.lstrip('*')) == expected, path
    assert '100% tests passed, 0 tests failed out of 123' in (directory / 'ctest.log').read_text()
    suite = load(stage / 'tests/ctest-suite.json')['tests']
    assert len(suite) == 123 and {'core_aov', 'core_data_preview', 'public_aov', 'public_interrupt'}.issubset({item['name'] for item in suite})
    log = (directory / 'Testing/Temporary/LastTest.log').read_text()
    assert 'Ran 17 tests' in log and 'Ran 20 tests' in log and 'Ran 19 tests' in log
    block = log.split('<end of output>', 1)[0]
    isolated = [json.loads(line) for line in block.splitlines() if line.startswith('{')]
    assert len(isolated) == 1 and isolated[0]['ok'] is True and len(isolated[0]['records']) == 77
    schema = Draft202012Validator(load(args.source / 'schemas/protocol-v1.schema.json'))
    for record in isolated[0]['records']:
        schema.validate(record)
    production_path = args.production / 'report.json'
    if (directory / 'production-report.json').exists():
        if not args.audit_output:
            raise FileExistsError('Preserve collected evidence; choose a new stage or explicit new audit output')
        assert digest(production_path) == digest(directory / 'production-report.json')
    else:
        shutil.copyfile(production_path, directory / 'production-report.json')
    report = load(production_path)
    assert (report['tasks'], report['new_previews'], report['data_views'], report['data_previews']) == (48, 288, 14, 168)
    assert report['colour_previews_equal_to_same_os_d095'] and not report['colour_sha_mismatches']
    assert report['data_repeats_sha_equal'] and report['actual_readback_and_encoding_checks'] == 408
    assert report['new_exe_sha256'] == digest(stage / 'bin/thumbgen')
    assert report['baseline_exe_sha256'] == digest(args.baseline_stage / 'bin/thumbgen')
    all_encoding = Counter()
    baseline_hashes = {}
    data_hashes = {}
    compared_colours = 0
    for run in report['runs']:
        assert run['process']['exit'] == 0 and not run['process']['timeout']
        expected_tasks = 20 if run['name'] == 'd095-colour-baseline' else 48
        assert len(run['records']) == expected_tasks + 1 and len(run['checks']) == expected_tasks * 6
        for record in run['records']:
            schema.validate(record)
        for check in run['checks']:
            code = check['encoding']
            assert digest(code['path']) == code['sha256']
            assert check['pixels']['readable']
            all_encoding[code['format']] += 1
            key = (check['id'], code['format'], check['pixels']['width'], check['pixels']['height'])
            if run['name'] == 'd095-colour-baseline':
                baseline_hashes[key] = code['sha256']
            elif check['id'].startswith('colour-'):
                assert baseline_hashes[key] == code['sha256'], key
                compared_colours += 1
            else:
                data_hashes[key] = code['sha256']
            if code['format'] == 'jpeg':
                assert code['standard_q90_luminance']
            elif code['format'] == 'webp':
                assert code['lossy_vp8'] and 'VP8L' not in code['riff_chunks']
            elif code['format'] == 'png':
                assert code['bit_depth'] == 8
    assert dict(all_encoding) == dict(jpeg=136, png=136, webp=136)
    compared_repeats = 0
    for (identity, *rest), expected in data_hashes.items():
        if identity.startswith('r2-'):
            assert data_hashes[('r1-' + identity[3:], *rest)] == expected
            compared_repeats += 1
    assert len(baseline_hashes) == compared_colours == 120 and len(data_hashes) == 168 and compared_repeats == 84
    windows_inputs = load(stage / 'reference-inputs/windows-mixed-report.json')['input_sha256']
    assert {item['windows_path']: item['sha256'] for item in report['source_inputs']} == windows_inputs
    frozen_repro = load(stage / 'reference-inputs/windows-repro-v1-report.json')['input_sha256']
    assert len(frozen_repro) == 1 and set(frozen_repro.values()).issubset(set(windows_inputs.values()))
    for item in report['source_inputs']:
        assert digest(item['original']) == digest(item['copied']) == item['sha256']
    for name, expected in report['references_sha256'].items():
        assert digest(stage / 'reference-inputs' / name) == expected
    artifacts = {}
    for line in (directory / 'artifacts.sha256').read_text().splitlines():
        expected, path = line.split(maxsplit=1)
        assert digest(stage / path.lstrip('*')) == expected
        artifacts[path] = expected
    sdk_count = 0
    if args.sdk:
        original_sdk = load(repo / '.deps/linux/validation-clean-sdk/sdk-rebuild.json')
        for name, expected in original_sdk['installed_sha256'].items():
            assert digest(args.sdk / name) == expected, name
            sdk_count += 1
    environment = dict(os.environ, LD_LIBRARY_PATH=str(stage / 'lib'))
    loader = subprocess.check_output(['ldd', str(stage / 'bin/thumbgen')], text=True, env=environment)
    assert 'not found' not in loader
    paths = sorted(set(re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader)))
    libraries = [dict(path=path, sha256=digest(path)) for path in paths if Path(path).is_file()]
    (directory / 'actual-loaded-libraries.json').write_text(json.dumps(libraries, indent=2) + '\n')
    summary = dict(platform=label, archive_commit=archive['commit'], source_files_verified=len(archive['sha256']),
                   ctest_passed=123, public_aov_cases=17, isolated_records_validated=77,
                   new_exe_sha256=digest(stage / 'bin/thumbgen'), old_exe_sha256=digest(args.baseline_stage / 'bin/thumbgen'),
                   new_artifacts=artifacts, production_origin=str(production_path), production_report_sha256=digest(production_path),
                   data_previews=168, data_repeats_sha_equal=True, colour_previews_identical_to_linux_d095=120,
                   source_count=20, original_source_sha_unchanged=True, schema_records=49, baseline_schema_records=21,
                   new_readbacks=288, baseline_readbacks=120, actual_encoding_counts=dict(all_encoding),
                   independently_compared_colour_sha=compared_colours, independently_compared_data_repeat_pairs=compared_repeats,
                   protected_files_verified=len(protected), sdk_files_verified=sdk_count,
                   limitations='Exploratory fixed ranges; no semantic/visual/performance, AOV02, minimum physical platform or release acceptance')
    output = args.audit_output or directory / 'aov-audit.json'
    if output.exists():
        raise FileExistsError(output)
    output.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: value for key, value in summary.items() if key != 'new_artifacts'}, indent=2))


if __name__ == '__main__':
    main()
