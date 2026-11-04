"""Bind per-OS test/reproduction evidence to an exact archive and immutable new stage."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess

from jsonschema import Draft202012Validator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, help='CentOS-only SDK install identity recheck')
    parser.add_argument('--readelf', default='readelf', help='Explicit ELF inspector, also usable on hosts without binutils')
    args = parser.parse_args()
    stage = args.stage.resolve()
    repo = stage.parents[2]
    label = os.environ['WSL_DISTRO_NAME']
    directory = stage / 'results' / label
    load = lambda path: json.loads(path.read_text())
    digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    archive = load(stage / 'archive-source.json')
    assert not (args.source / '.git').exists()
    assert digest(repo / archive['archive']) == archive['archive_sha256']
    for path, expected in archive['sha256'].items():
        assert digest(args.source / path) == expected, path
    protected = load(stage / 'preserved-evidence.json')
    for path, expected in protected.items():
        assert digest(repo / path) == expected, path
    historical_manifests = [path for path in (repo / '.deps/linux').glob('validation*/results/CentOS-7.9/artifacts.sha256')
                            if stage not in path.parents]
    historical_entries = 0
    for manifest in historical_manifests:
        for line in manifest.read_text().splitlines():
            expected, path = line.split(maxsplit=1)
            assert digest(manifest.parents[2] / path.lstrip('*')) == expected, path
            historical_entries += 1
    assert len(historical_manifests) == 12 and historical_entries == 295
    for line in (stage / 'PRODUCT-FROZEN.sha256').read_text().splitlines():
        expected, path = line.split(maxsplit=1)
        assert digest(Path(path.lstrip('*'))) == expected, path
    artifacts = {}
    for line in (directory / 'artifacts.sha256').read_text().splitlines():
        expected, path = line.split(maxsplit=1)
        path = path.lstrip('*')
        assert digest(stage / path) == expected, path
        artifacts[path] = expected
    tests = load(stage / 'tests/ctest-suite.json')['tests']
    assert len(tests) == 120 and any(item['name'] == 'core_quantize' for item in tests)
    test_log = (directory / 'ctest.log').read_text()
    assert '100% tests passed, 0 tests failed out of 120' in test_log
    native_log = (directory / 'Testing/Temporary/LastTest.log').read_text()
    assert '200775 values, 12 chunk sizes, 8 alignments matched independent oracle' in native_log
    assert native_log.count('OK (skipped=1)') == 2
    repro = load(directory / 'repro-report.json')
    schema = Draft202012Validator(load(args.source / 'schemas/protocol-v1.schema.json'))
    assert repro['capabilities']['quantization'] == 'unorm8-half-up-v1'
    schema.validate(repro['capabilities'])
    assert (repro['tasks'], repro['previews'], repro['schema_records']) == (12, 72, 24)
    assert Counter((run['threads'], run['repeat']) for run in repro['runs']) == Counter((t, r) for t in (1, 2, 4, 8) for r in (1, 2, 3))
    reference = stage / 'input-references'
    assert digest(reference / 'repro-corpus.json') == repro['corpus_sha256']
    assert digest(reference / 'windows-quantize-repro-report.json') == repro['reference_report_sha256']
    assert digest(repro['original_path']) == digest(repro['copy_path']) == repro['source_sha256']
    baseline = [item['sha256'] for item in repro['runs'][0]['encodings']]
    for run in repro['runs']:
        assert run['unit']['valid'] and run['matches_first_thread1']
        assert [item['sha256'] for item in run['encodings']] == baseline
        for record in run['unit']['records']:
            schema.validate(record)
            if record['type'] == 'result':
                assert record['execution']['threads'] == run['threads']
    pressure = load(directory / 'report.json')
    schema.validate(pressure['capabilities'])
    assert repro['product_sha256'] == pressure['executable_sha256'] == digest(stage / 'bin/thumbgen')
    assert repro['helper_sha256'] == pressure['native_image_validator']['sha256'] == digest(pressure['native_image_validator']['path'])
    copied = load(directory / 'source-copy.json')['inputs']
    for item in copied:
        assert digest(item['read_source']) == digest(item['linux_copy']) == item['sha256']
    # Old native pressure reports and their output paths remain present; no output is rewritten.
    old_directory = repo / '.deps/linux/validation-pressure-linux/results' / label
    old_storage = load(old_directory / 'storage.json')
    assert digest(old_storage['report_origin']) == old_storage['report_sha256']
    old_report = load(Path(old_storage['report_origin']))
    old_outputs = [Path(output['path']) for item in old_report['profiles'] + old_report['batches']
                   for record in item['unit']['records'] if record['type'] == 'result' for output in record['outputs']]
    assert len(old_outputs) == 264 and all(path.is_file() for path in old_outputs)
    sdk_count = 0
    if args.sdk:
        previous_sdk = load(repo / '.deps/linux/validation-clean-sdk/sdk-rebuild.json')
        for path, expected in previous_sdk['installed_sha256'].items():
            assert digest(args.sdk / path) == expected, path
            sdk_count += 1
    cpu_fields = dict(line.split(':', 1) for line in Path('/proc/cpuinfo').read_text().split('\n\n')[0].splitlines() if ':' in line)
    cpu = {key.strip(): value.strip() for key, value in cpu_fields.items() if key.strip() in ('vendor_id', 'model name', 'flags')}
    (directory / 'elf-notes.txt').write_text(subprocess.check_output([args.readelf, '-h', '-n', str(stage / 'bin/thumbgen')], text=True))
    report = dict(platform=label, archive_commit=archive['commit'], archive_sha256=archive['archive_sha256'],
                  source_files_verified=len(archive['sha256']), product_sha256=digest(stage / 'bin/thumbgen'),
                  product_identity=repro['product_identity'], cpu=cpu, capabilities=repro['capabilities'],
                  readelf_inspector=args.readelf,
                  protected_evidence_files_verified=len(protected), historical_stage_manifests_verified=12,
                  historical_stage_entries_verified=295, previous_pressure_outputs_retained=264,
                  sdk_installed_files_verified=sdk_count, new_artifacts=artifacts, ctest_passed=120,
                  public_suites_windows_only_skips=2, repro_tasks=12, repro_previews=72, repro_schema_records=24,
                  repro_all_thread_sha_equal=True, repro_source_sha256=repro['source_sha256'],
                  repro_baseline_output_sha256=baseline, pressure_tasks=108, pressure_previews=264, pressure_schema_records=124,
                  encoding_counts=load(directory / 'encoding-audit.json')['counts'],
                  limits='WSL sampling; no physical-minimum, shared-storage, visual/complex-colour QA or default-threshold acceptance')
    (directory / 'quantize-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('cpu', 'new_artifacts', 'capabilities')}, indent=2))


if __name__ == '__main__':
    main()
