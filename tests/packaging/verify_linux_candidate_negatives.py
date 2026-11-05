"""Mutate PRIVATE copies only; require failures without replacing original evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--spec',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args(); spec=json.loads(a.spec.read_text()); out=a.output
out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
original=Path(spec['archive']); original_sha=sha(original)
results=[]
def expect(name,command,reason):
    result=subprocess.run(command,capture_output=True,text=True)
    (out/(name+'.stdout')).write_text(result.stdout)
    (out/(name+'.stderr')).write_text(result.stderr)
    row=dict(name=name,command=command,exit=result.returncode,required_reason=reason,
             passed=result.returncode!=0 and reason in result.stdout+result.stderr)
    results.append(row); (out/'report.json').write_text(json.dumps(results,indent=2)+'\n')
    assert row['passed'],row
def validator(archive):
    command=spec['validator'][:]; command[command.index('--archive')+1]=str(archive)
    return command
def candidate(name,mutation):
    directory=out/name; directory.mkdir()
    with tarfile.open(original) as archive: archive.extractall(directory)
    root=next(directory.iterdir()); mutation(root)
    sums=[sha(path)+'  '+path.relative_to(root).as_posix() for path in sorted(root.rglob('*')) if path.is_file() and path.name!='SHA256SUMS']
    (root/'SHA256SUMS').write_text('\n'.join(sums)+'\n')
    archive=out/(name+'.tar.gz')
    with tarfile.open(archive,'w:gz') as target:target.add(root,arcname=root.name)
    Path(str(archive)+'.sha256').write_text(sha(archive)+'  '+archive.name+'\n')
    return archive
wrong=out/'wrong-digest.tar.gz';shutil.copyfile(original,wrong)
Path(str(wrong)+'.sha256').write_text('0'*64+'  '+wrong.name+'\n')
expect('wrong-archive-digest',validator(wrong),'Archive checksum mismatch')
def missing_notice(root):
    manifest=json.loads((root/'DEPENDENCIES.json').read_text())
    item=next(c for c in manifest['dependency_components'] if c['name']=='LittleCMS2-shared')
    (root/item['notices'][0]['file']).unlink()
expect('missing-notice',validator(candidate('notice-removed',missing_notice)),'No such file')
def extra(root):
    shutil.copyfile(root/'lib/liblcms2.so.2.20.0',root/'lib/libunreviewed.so.1')
expect('unknown-library',validator(candidate('unknown-library',extra)),'Library payload differs')
stage=out/'spoofed-stage';shutil.copytree(spec['stage'],stage,symlinks=True)
library=stage/'lib/liblcms2.so.2.20.0'
data=bytearray(library.read_bytes());data[-1]^=1;library.write_bytes(data)
evidence=json.loads((stage/'build-evidence.json').read_text())
evidence['staged_artifacts']['lib/liblcms2.so.2.20.0']=sha(library)
evidence['staged_artifacts']['lib/liblcms2.so.2']=sha(library)
(stage/'build-evidence.json').write_text(json.dumps(evidence))
command=spec['packager'][:];command[command.index('--validation')+1]=str(stage)
expect('self-authored-stage-map',command,'Staged library differs from linked dependency')
assert sha(original)==original_sha
print(json.dumps(dict(passed=len(results),original_archive_unchanged=True,report=str(out/'report.json'))))
