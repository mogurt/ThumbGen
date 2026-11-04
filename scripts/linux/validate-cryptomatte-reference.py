"""Validate the frozen official Cryptomatte raw oracle with stdlib PNG decoding only."""
import argparse
import ast
from collections import Counter
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import signal
import struct
import subprocess
import zlib

from jsonschema import Draft202012Validator


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','stage','references','assets','helper','directory'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.directory.mkdir(exist_ok=False)
    reference_path=args.references/'official-report.json';reference=json.loads(reference_path.read_text())
    assert digest(reference_path)=='779598317acb2d91a461cabc2a560468046cae9234dd8e03cfa7da3e41b6ec4e'
    catalog=json.loads((args.references/'cryptomatte-assets.json').read_text())
    assert catalog['commit']==reference['upstream_commit']=='968d5e4b6171e29ba5f89d554117132a164e747e'
    inputs=args.directory/'inputs';inputs.mkdir();sources={}
    for name,info in catalog['files'].items():
        original=args.assets/name;assert digest(original)==info['sha256']==reference['source_sha256'][name]
        target=inputs/name;shutil.copyfile(original,target);assert digest(target)==digest(original)==info['sha256']
        target.chmod(0o444);sources[name]=dict(original=str(original),copied=str(target),sha256=info['sha256'])
    inputs.chmod(0o555)
    expected={}
    for item in reference['expected']:
        path=args.references/'expected'/PureWindowsPath(item['path']).name
        assert digest(path)==item['sha256'] and path.stat().st_size==item['width']*item['height']*item['channels']
        expected[(item['view'],item['background'])]=dict(item,path=str(path))
    assert len(expected)==16 and len(reference['views'])==8
    png_source=args.source/'tests/test_cryptomatte_cli.py';tree=ast.parse(png_source.read_text())
    functions=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='png'];assert len(functions)==1
    namespace=dict(Path=Path,struct=struct,zlib=zlib)
    exec(compile(ast.Module(body=functions,type_ignores=[]),str(png_source),'exec'),namespace)
    png=namespace['png']
    spec=importlib.util.spec_from_file_location('encoding',Path(__file__).with_name('verify-preview-encoding.py'))
    encoding=importlib.util.module_from_spec(spec);spec.loader.exec_module(encoding)
    request=copy.deepcopy(json.loads((args.references/'official-jobs.json').read_text()))
    assert len(request['jobs'])==24
    for job in request['jobs']:
        job['input']=sources[PureWindowsPath(job['input']).name]['copied']
        for output in job['outputs']:output['path']=str(args.directory/PureWindowsPath(output['path']).name)
    Draft202012Validator(json.loads((args.source/'schemas/jobs-v1.schema.json').read_text())).validate(request)
    jobs=args.directory/'jobs.json';jobs.write_text(json.dumps(request,indent=2)+'\n')
    env=dict(os.environ,LD_LIBRARY_PATH=str(args.stage/'lib'),OCIO='cryptomatte-intentionally-invalid-parent.ocio')
    process=subprocess.Popen([str(args.stage/'bin/thumbgen'),'batch','--manifest',str(jobs),'--json'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
    timeout=False
    try:stdout,stderr=process.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        timeout=True;os.killpg(process.pid,signal.SIGKILL);stdout,stderr=process.communicate()
    (args.directory/'stdout.jsonl').write_text(stdout);(args.directory/'stderr.txt').write_text(stderr)
    status=dict(exit=process.returncode,timeout=timeout);(args.directory/'process.json').write_text(json.dumps(status,indent=2)+'\n')
    assert process.returncode==0 and not timeout
    records=[json.loads(line) for line in stdout.splitlines() if line.strip()]
    validator=Draft202012Validator(json.loads((args.source/'schemas/protocol-v1.schema.json').read_text()))
    assert len(records)==25 and records[-1]['success']==24
    for row in records:validator.validate(row)
    by_id={row['id']:row for row in records if row['type']=='result'}
    report=dict(exe_sha256=digest(args.stage/'bin/thumbgen'),helper_sha256=digest(args.helper),reference_report_sha256=digest(reference_path),
                png_decoder_source_sha256=digest(png_source),png_decoder_ast_sha256=hashlib.sha256(ast.dump(functions[0]).encode()).hexdigest(),
                sources=sources,expected=list(expected.values()),records=records,process=status,checks=[],full_png_comparisons=[],repeat_pairs=[],errors=[])
    for view in reference['views']:
        for suffix in ('-r1','-r2','-transparent'):
            record=by_id[view['id']+suffix]
            if record['visualization']['invalid_pixels']!=view['invalid_pixels'] or record['visualization']['clipped_components']!=view['clipped_components']:
                report['errors'].append(dict(id=record['id'],kind='reference counters differ',expected=view,actual=record['visualization']))
            assert record['colour']['config_basis']=='none'
            for output in record['outputs']:
                code=encoding.inspect_encoding(output['path'])
                read=subprocess.run([str(args.helper),output['path']],env=env,capture_output=True,text=True)
                pixels=json.loads(read.stdout);assert read.returncode==0 and pixels['readable']
                assert (pixels['width'],pixels['height'])==(output['width'],output['height'])
                report['checks'].append(dict(id=record['id'],encoding=code,pixels=pixels))
            target=record['outputs'][-1]['path'];label='transparent' if suffix=='-transparent' else 'black';oracle=expected[(view['id'],label)]
            width,height,pixels=png(target);raw=bytes(value for pixel in pixels for value in pixel)
            comparison=dict(id=record['id'],output=target,reference_raw=oracle['path'],expected_sha256=oracle['sha256'],
                            actual_raw_sha256=hashlib.sha256(raw).hexdigest(),width=width,height=height,channels=len(pixels[0]),
                            equal=(width,height,len(pixels[0]))==(oracle['width'],oracle['height'],oracle['channels']) and raw==Path(oracle['path']).read_bytes())
            report['full_png_comparisons'].append(comparison)
            if not comparison['equal']:
                actual=args.directory/(record['id']+'-actual.raw');actual.write_bytes(raw)
                oracle_bytes=Path(oracle['path']).read_bytes()
                diffs=[index for index,(a,b) in enumerate(zip(raw,oracle_bytes)) if a!=b]
                report['errors'].append(dict(id=record['id'],kind='full PNG pixel mismatch',actual_raw=str(actual),different_bytes=len(diffs),first_offsets=diffs[:16]))
            (args.directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        first=by_id[view['id']+'-r1']['outputs'];second=by_id[view['id']+'-r2']['outputs']
        for a,b in zip(first,second):
            one,two=digest(a['path']),digest(b['path']);report['repeat_pairs'].append(dict(one=a['path'],two=b['path'],sha256=one,equal=one==two))
    cornell=[row for row in records if row.get('id','').startswith('cornellBox')]
    locks=[row['selection']['lock_status'] for row in cornell]
    assert len(cornell)==6 and locks.count('created')==1 and locks.count('reused')==5
    assert all(row['selection']['locked']==cornell[0]['selection']['locked'] for row in cornell)
    assert len(report['checks'])==120 and len(report['full_png_comparisons'])==24 and len(report['repeat_pairs'])==56
    assert all(digest(row['original'])==digest(row['copied'])==row['sha256'] for row in sources.values())
    report.update(tasks=24,previews=120,schema_records=25,full_png_comparisons_count=24,repeat_pairs_count=56,
                  cornell_lock_statuses=locks,source_sha256_unchanged=True,encoding_counts=dict(Counter(c['encoding']['format'] for c in report['checks'])))
    (args.directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert not report['errors'] and all(pair['equal'] for pair in report['repeat_pairs']), 'Official reference mismatches retained in report'
    print(json.dumps(dict(tasks=24,previews=120,full_png_comparisons=24,repeat_pairs=56,cornell_locks=locks)))


if __name__=='__main__':main()
