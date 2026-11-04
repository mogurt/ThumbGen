"""Audit a frozen categorical stage, same-OS production byte references and all benchmark rounds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess

from jsonschema import Draft202012Validator


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stage', 'source', 'production', 'benchmark', 'previous-stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expected-count', type=int, required=True)
    parser.add_argument('--sdk', type=Path)
    args = parser.parse_args()
    stage = args.stage.resolve(); repo = stage.parents[2]
    label = os.environ['WSL_DISTRO_NAME']; result = stage / 'results' / label
    load = lambda path: json.loads(path.read_text())
    archive = load(stage / 'archive-source.json')
    assert digest(stage / 'source.tar') == archive['archive_sha256']
    assert all(digest(args.source / path) == expected for path, expected in archive['sha256'].items())
    protected = load(stage / 'preserved-evidence.json')
    for path, expected in protected.items(): assert digest(repo / path) == expected, path
    for line in (stage / 'PRODUCT-FROZEN.sha256').read_text().splitlines():
        expected, path = line.split(maxsplit=1); assert digest(path.lstrip('*')) == expected
    suite = load(stage / 'tests/ctest-suite.json')['tests']
    assert len(suite) == args.expected_count
    assert {'core_categorical','core_aov','core_data_preview','public_aov','public_interrupt'}.issubset({item['name'] for item in suite})
    assert f'100% tests passed, 0 tests failed out of {args.expected_count}' in (result / 'ctest.log').read_text()
    log = (result / 'Testing/Temporary/LastTest.log').read_text()
    assert 'Ran 17 tests' in log and 'Ran 20 tests' in log and 'Ran 19 tests' in log
    native = [json.loads(line) for line in log.split('<end of output>',1)[0].splitlines() if line.startswith('{')]
    assert len(native) == 1 and native[0]['ok'] and len(native[0]['records']) == 77
    schema = Draft202012Validator(load(args.source / 'schemas/protocol-v1.schema.json'))
    for record in native[0]['records']: schema.validate(record)
    production = load(args.production / 'report.json')
    assert production['previews'] == production['compared_previous_output_sha'] == production['actual_readbacks'] == 288
    assert production['schema_records'] == 49 and production['colour_d095_sha_equal'] == 120 and production['repeated_data_pairs'] == 84
    assert not production['mismatches'] and production['source_sha256_unchanged'] and not production['old_cli_rerun']
    assert production['exe_sha256'] == digest(stage / 'bin/thumbgen')
    assert production['previous_exe_sha256'] == digest(args.previous_stage / 'bin/thumbgen')
    old = load(Path(production['previous_report']))
    assert digest(Path(production['previous_report'])) == production['previous_report_sha256']
    old_maps = {}
    for run in old['runs']:
        table = {}
        for check in run['checks']:
            code, pixels = check['encoding'], check['pixels']; assert digest(code['path']) == code['sha256']
            table[(check['id'],code['format'],pixels['width'],pixels['height'])] = code['sha256']
        old_maps[run['name']] = table
    fresh = {}
    for check in production['checks']:
        code, pixels = check['encoding'], check['pixels']; assert pixels['readable'] and digest(code['path']) == code['sha256']
        key = (check['id'],code['format'],pixels['width'],pixels['height']); fresh[key] = code['sha256']
        assert old_maps['aov-mixed'][key] == code['sha256']
        if check['id'].startswith('colour-'): assert old_maps['d095-colour-baseline'][key] == code['sha256']
    assert len(fresh) == 288
    pairs = 0
    for (identity, *rest), sha in fresh.items():
        if identity.startswith('r2-'):
            assert fresh[('r1-'+identity[3:],*rest)] == sha; pairs += 1
    assert pairs == 84
    expected_inputs = load(stage / 'reference-inputs/windows-mixed-report.json')['input_sha256']
    assert {row['windows_path']:row['sha256'] for row in production['sources']} == expected_inputs
    for row in production['sources']: assert digest(row['original']) == digest(row['copied']) == row['sha256']
    for record in production['records']: schema.validate(record)
    benchmark = load(args.benchmark / 'report.json')
    assert benchmark['outputs_equal'] and benchmark['sampled_rss_not_hard_limit']
    assert digest(args.source / 'benchmarks/categorical.py') == benchmark['harness_sha256']
    assert benchmark['original_exe_sha256'] == dict(before=digest(args.previous_stage/'bin/thumbgen'),after=digest(stage/'bin/thumbgen'),fixture=digest(stage/'bin/thumbgen_categorical_tests'))
    assert len(benchmark['runs']) == 6 and len(benchmark['boundaries']) == 4
    assert [(run['repeat'],run['name']) for run in benchmark['runs']] == [(0,'before'),(0,'after'),(1,'after'),(1,'before'),(2,'before'),(2,'after')]
    reference_hashes = benchmark['runs'][0]['hashes']
    assert len(reference_hashes) == 24
    comparison_previews = 0; benchmark_schema_records = 0
    for run in benchmark['runs'] + benchmark['boundaries']:
        for record in run['records']: schema.validate(record); benchmark_schema_records += 1
        assert not run['process']['timeout']
        if run in benchmark['runs']:
            assert run['process']['exit'] == 0 and run['hashes'] == reference_hashes and run['tasks'] == 4 and run['memory_mib'] == 4096
            comparison_previews += len(run['encoding'])
        elif run['name'] == 'after':
            assert run['process']['exit'] == 0 and len(run['hashes']) == 6
        for code in run['encoding']:
            assert digest(code['path']) == code['sha256']
            if code['format']=='jpeg': assert code['standard_q90_luminance']
            elif code['format']=='webp': assert code['lossy_vp8']
            elif code['format']=='png': assert code['bit_depth']==8
    assert comparison_previews == 144
    assert {(r['size'],r['memory_mib']) for r in benchmark['boundaries'] if r['name']=='after'} == {(4096,1024),(8192,8192)}
    for path, sha in benchmark['input_sha256'].items(): assert digest(path)==sha
    for name in ('before','after'):
        for metric in ('wall_ms','sampled_peak_tree_rss_mib'):
            assert statistics.median(r['process'][metric] for r in benchmark['runs'] if r['name']==name) == benchmark['summary'][name][metric]
    artifacts = {}
    for line in (result/'artifacts.sha256').read_text().splitlines():
        sha,path=line.split(maxsplit=1); assert digest(stage/path)==sha; artifacts[path]=sha
    sdk_count=0
    if args.sdk:
        sdk=load(repo/'.deps/linux/validation-clean-sdk/sdk-rebuild.json')
        for path,sha in sdk['installed_sha256'].items(): assert digest(args.sdk/path)==sha; sdk_count+=1
    env=dict(os.environ,LD_LIBRARY_PATH=str(stage/'lib'))
    loader=subprocess.check_output(['ldd',str(stage/'bin/thumbgen')],env=env,text=True)
    assert 'not found' not in loader
    libraries=[dict(path=p,sha256=digest(p)) for p in sorted(set(re.findall(r'(?:=>\s*)?(/[^\s()]+)',loader))) if Path(p).is_file()]
    (result/'actual-loaded-libraries.json').write_text(json.dumps(libraries,indent=2)+'\n')
    for name, path in (('production-report.json',args.production/'report.json'),('benchmark-report.json',args.benchmark/'report.json')):
        if (result/name).exists(): raise FileExistsError(result/name)
        shutil.copyfile(path,result/name)
    summary=dict(platform=label,archive_commit=archive['commit'],ctest_passed=len(suite),public_aov_cases=17,isolated_records=77,
                 artifacts=artifacts,production_origin=str(args.production),benchmark_origin=str(args.benchmark),
                 production_previews=288,production_compared_to_previous=288,colour_d095_sha_equal=120,data_repeat_pairs=84,
                 benchmark_comparison_previews=comparison_previews,benchmark_schema_records=benchmark_schema_records,
                 benchmark_summary=benchmark['summary'],all_rounds=[dict(name=r['name'],repeat=r['repeat'],**r['process']) for r in benchmark['runs']],
                 boundaries=[dict(name=r['name'],size=r['size'],memory_mib=r['memory_mib'],process=r['process']) for r in benchmark['boundaries']],
                 protected_files_verified=len(protected),sdk_files_verified=sdk_count,source_files_verified=len(archive['sha256']),
                 limits='Full native span decoded; 10ms summed RSS can miss peaks/double-count shared pages; not hard RSS, visual QA, defaults or release acceptance')
    (result/'categorical-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('artifacts','all_rounds')},indent=2))


if __name__ == '__main__': main()
