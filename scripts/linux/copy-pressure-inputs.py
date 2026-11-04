"""Copy the twelve frozen Windows inputs to a fresh Linux directory and prove byte identity."""
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil

parser=argparse.ArgumentParser()
parser.add_argument('--manifest',type=Path,required=True)
parser.add_argument('--inventory',type=Path,required=True)
parser.add_argument('--windows-report',type=Path,required=True)
parser.add_argument('--destination',type=Path,required=True)
args=parser.parse_args()
rows=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
reference=json.loads(args.windows_report.read_text(encoding='utf-8'))
if len(rows)!=12 or set(row['id'] for row in rows)!=set(reference['input_sha256']):
    raise RuntimeError('Expected exact twelve Windows reference inputs')
args.destination.mkdir(exist_ok=False)
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
copied=[];provenance=[]
for row in rows:
    original=PureWindowsPath(row['path'])
    path=Path('/mnt')/original.drive[0].lower()/Path(*original.parts[1:])
    before=digest(path)
    if before!=reference['input_sha256'][row['id']]:
        raise RuntimeError('Windows source drift: '+row['id'])
    destination=args.destination/path.name
    shutil.copyfile(path,destination)
    if digest(destination)!=before or digest(path)!=before:
        raise RuntimeError('Source/copy checksum mismatch')
    destination.chmod(0o444)
    copied.append(dict(id=row['id'],path=str(destination.resolve())))
    provenance.append(dict(id=row['id'],windows_path=row['path'],read_source=str(path),
                           linux_copy=str(destination.resolve()),sha256=before,bytes_identical=True))
inventory=args.inventory.read_bytes()
if reference.get('fixture_inventory',{}).get('sha256') and hashlib.sha256(inventory).hexdigest()!=reference['fixture_inventory']['sha256']:
    raise RuntimeError('Windows inventory differs from reference')
(args.destination/'manifest.json').write_text(json.dumps(copied,indent=2)+'\n')
(args.destination/'inventory.json').write_bytes(inventory)
(args.destination/'source-copy.json').write_text(json.dumps(dict(inputs=provenance,windows_report_sha256=digest(args.windows_report),
    inventory_sha256=hashlib.sha256(inventory).hexdigest(),storage='separate Linux ext4 copies; original Windows files only read'),indent=2)+'\n')
for path in args.destination.iterdir():path.chmod(0o444)
args.destination.chmod(0o555)
print(args.destination)
