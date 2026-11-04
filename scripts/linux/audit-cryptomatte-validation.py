"""Audit a frozen full Cryptomatte stage, unchanged mainline and independent raw oracle receipts."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from jsonschema import Draft202012Validator


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','stage','mainline','official','previous-stage'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--expected-count',type=int,required=True)
    parser.add_argument('--sdk',type=Path)
    args=parser.parse_args();stage=args.stage.resolve();repo=stage.parents[2]
    result=stage/'results'/os.environ['WSL_DISTRO_NAME'];load=lambda p:json.loads(p.read_text())
    archive=load(stage/'archive-source.json')
    assert digest(stage/'source.tar')==archive['archive_sha256']
    assert all(digest(args.source/path)==sha for path,sha in archive['sha256'].items())
    protected=load(stage/'preserved-evidence.json')
    for path,sha in protected.items():assert digest(repo/path)==sha,path
    for line in (stage/'PRODUCT-FROZEN.sha256').read_text().splitlines():
        sha,path=line.split(maxsplit=1);assert digest(path.lstrip('*'))==sha
    suite=load(stage/'tests/ctest-suite.json')['tests']
    assert len(suite)==args.expected_count and {'core_cryptomatte','public_cryptomatte','core_selected_input','public_aov','public_interrupt'}.issubset({t['name'] for t in suite})
    assert f'100% tests passed, 0 tests failed out of {args.expected_count}' in (result/'ctest.log').read_text()
    log=(result/'Testing/Temporary/LastTest.log').read_text()
    for case,count in (('public_cryptomatte',20),('public_aov',17)):
        block=log.split('Test: '+case+'\n',1)[1].split('<end of output>',1)[0]
        assert f'Ran {count} tests' in block
    schema=Draft202012Validator(load(args.source/'schemas/protocol-v1.schema.json'))
    isolated=[json.loads(line) for line in log.split('<end of output>',1)[0].splitlines() if line.startswith('{')]
    assert len(isolated)==1 and isolated[0]['ok'] and len(isolated[0]['records'])==77
    for record in isolated[0]['records']:schema.validate(record)
    mainline=load(args.mainline/'report.json')
    assert mainline['exe_sha256']==digest(stage/'bin/thumbgen') and mainline['previous_exe_sha256']==digest(args.previous_stage/'bin/thumbgen')
    assert (mainline['tasks'],mainline['previews'],mainline['actual_readbacks'])==(48,288,288) and not mainline['mismatches']
    assert mainline['colour_d095_sha_equal']==120 and mainline['repeated_data_pairs']==84 and not mainline['old_cli_rerun']
    old=load(Path(mainline['previous_report']));assert digest(mainline['previous_report'])==mainline['previous_report_sha256']
    spec=importlib.util.spec_from_file_location('reference_utility',Path(__file__).with_name('validate-categorical-production.py'))
    utility=importlib.util.module_from_spec(spec);spec.loader.exec_module(utility)
    colour=Path(mainline['colour_reference_report']);assert digest(colour)==mainline['colour_reference_report_sha256']
    _,_,old_runs=utility.previous_references(old,colour)
    tables={}
    for run in old_runs:
        table={}
        for check in run['checks']:
            code,pixel=check['encoding'],check['pixels'];assert digest(code['path'])==code['sha256']
            table[(check['id'],code['format'],pixel['width'],pixel['height'])]=code['sha256']
        tables[run['name']]=table
    for check in mainline['checks']:
        code,pixel=check['encoding'],check['pixels'];key=(check['id'],code['format'],pixel['width'],pixel['height'])
        assert digest(code['path'])==code['sha256']==tables['aov-mixed'][key] and pixel['readable']
        if check['id'].startswith('colour-'):assert code['sha256']==tables['d095-colour-baseline'][key]
    expected_sources=load(stage/'reference-inputs/windows-mixed-report.json')['input_sha256']
    assert {row['windows_path']:row['sha256'] for row in mainline['sources']}==expected_sources
    for row in mainline['sources']:assert digest(row['original'])==digest(row['copied'])==row['sha256']
    for row in mainline['records']:schema.validate(row)
    official=load(args.official/'report.json')
    reference=load(stage/'reference-inputs/official-report.json')
    assert official['reference_report_sha256']==digest(stage/'reference-inputs/official-report.json')=='779598317acb2d91a461cabc2a560468046cae9234dd8e03cfa7da3e41b6ec4e'
    assert (official['tasks'],official['previews'],official['schema_records'])==(24,120,25) and not official['errors']
    assert official['exe_sha256']==mainline['exe_sha256'] and official['helper_sha256']==mainline['helper_sha256']
    assert official['png_decoder_source_sha256']==digest(args.source/'tests/test_cryptomatte_cli.py')
    for row in official['records']:schema.validate(row)
    assert len(official['checks'])==120 and len(official['full_png_comparisons'])==24 and len(official['repeat_pairs'])==56
    for check in official['checks']:
        code,pixel=check['encoding'],check['pixels'];assert digest(code['path'])==code['sha256'] and pixel['readable']
        if code['format']=='jpeg':assert code['standard_q90_luminance']
        elif code['format']=='png':assert code['bit_depth']==8
        elif code['format']=='webp':assert code['lossy_vp8']
    raw_manifest={(row['view'],row['background']):row for row in reference['expected']}
    for comparison in official['full_png_comparisons']:
        assert comparison['equal'] and comparison['actual_raw_sha256']==comparison['expected_sha256']==digest(comparison['reference_raw'])
        identity=comparison['id'];label='transparent' if identity.endswith('-transparent') else 'black'
        view=identity.rsplit('-',1)[0];expected=raw_manifest[(view,label)]
        assert (comparison['width'],comparison['height'],comparison['channels'])==(expected['width'],expected['height'],expected['channels'])
        assert comparison['expected_sha256']==expected['sha256']
    views={view['id']:view for view in reference['views']}
    for record in official['records']:
        if record['type']!='result':continue
        view=views[record['id'].rsplit('-',1)[0]]
        assert record['visualization']['invalid_pixels']==view['invalid_pixels'] and record['visualization']['clipped_components']==view['clipped_components']
        assert record['cryptomatte']['manifest']['used'] is False
    for pair in official['repeat_pairs']:assert pair['equal'] and digest(pair['one'])==digest(pair['two'])==pair['sha256']
    assert official['cornell_lock_statuses'].count('created')==1 and official['cornell_lock_statuses'].count('reused')==5
    for name,row in official['sources'].items():assert digest(row['original'])==digest(row['copied'])==row['sha256']==reference['source_sha256'][name]
    artifacts={}
    for line in (result/'artifacts.sha256').read_text().splitlines():
        sha,path=line.split(maxsplit=1);assert digest(stage/path)==sha;artifacts[path]=sha
    sdk_count=0
    if args.sdk:
        sdk=load(repo/'.deps/linux/validation-clean-sdk/sdk-rebuild.json')
        for path,sha in sdk['installed_sha256'].items():assert digest(args.sdk/path)==sha;sdk_count+=1
    env=dict(os.environ,LD_LIBRARY_PATH=str(stage/'lib'))
    loader=subprocess.check_output(['ldd',str(stage/'bin/thumbgen')],env=env,text=True);assert 'not found' not in loader
    libraries=[dict(path=p,sha256=digest(p)) for p in sorted(set(re.findall(r'(?:=>\s*)?(/[^\s()]+)',loader))) if Path(p).is_file()]
    (result/'actual-loaded-libraries.json').write_text(json.dumps(libraries,indent=2)+'\n')
    for name,path in (('mainline-report.json',args.mainline/'report.json'),('official-report.json',args.official/'report.json')):
        if (result/name).exists():raise FileExistsError(result/name)
        shutil.copyfile(path,result/name)
    summary=dict(platform=os.environ['WSL_DISTRO_NAME'],archive_commit=archive['commit'],ctest_passed=len(suite),public_cryptomatte_cases=20,public_aov_cases=17,isolated_records=77,
                 artifacts=artifacts,mainline_previews=288,mainline_compared_previous=288,colour_d095_equal=120,data_repeat_pairs=84,mainline_schema_records=49,
                 official_sources=6,official_views=8,official_tasks=24,official_previews=120,official_full_png_exact=24,official_repeat_pairs=56,official_schema_records=25,
                 cornell_lock_statuses=official['cornell_lock_statuses'],total_native_output_readbacks=408,expected_raw_count=16,
                 official_encoding_counts=official['encoding_counts'],protected_files_verified=len(protected),sdk_files_verified=sdk_count,source_files_verified=len(archive['sha256']),
                 mainline_origin=str(args.mainline),official_origin=str(args.official),limits='Coverage overview only; no name extraction/manifest parsing, Deep, visual QA or release acceptance')
    (result/'cryptomatte-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k!='artifacts'},indent=2))


if __name__=='__main__':main()
