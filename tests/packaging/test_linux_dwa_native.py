"""Real portable-DWA native/install relocation and rejection fixtures."""
import argparse
import unittest
import json,sys,shutil
from pathlib import Path
if __name__ != '__main__':
    raise unittest.SkipTest('Explicit Linux native prefix run required: --dwa-root and --work NEW_DIRECTORY')
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dwa-root', required=True, type=Path)
parser.add_argument('--work', required=True, type=Path)
args=parser.parse_args()
repo=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(repo/'scripts/linux'))
import runtime_overlays as m
root=args.work.resolve();root.mkdir(parents=True,exist_ok=False)
dwa=args.dwa_root.resolve()
results=[]
for case in ('relocated-positive','alias-unknown','unknown-archive','changed-archive'):
 prefix=root/case;shutil.copytree(dwa/'prefix',prefix,symlinks=True)
 before={p.relative_to(prefix).as_posix():m.sha(p) for p in prefix.rglob('*') if p.is_file()}
 if case=='alias-unknown':
  alias=prefix/'lib/libOpenImageIO.so.3.2';alias.unlink();shutil.copyfile(prefix/'lib/libOpenImageIO.so.3.2.1',prefix/'lib/payload.bin');alias.symlink_to('payload.bin')
 elif case=='unknown-archive':shutil.copyfile(prefix/'lib/libOpenEXRCore_v_3_5_0_OpenImageIO_v3_2.a',prefix/'lib/payload')
 elif case=='changed-archive':
  p=prefix/'lib/libOpenEXRCore_v_3_5_0_OpenImageIO_v3_2.a';p.write_bytes(p.read_bytes()+b'changed')
 proposed={p.relative_to(prefix).as_posix():m.sha(p) for p in prefix.rglob('*') if p.is_file()}
 item=dict(case=case,proposed_receipt=proposed,before=before)
 try:
  install=m.audit_installations(prefix,[(dwa/(name+'-build'),prefix) for name in ('OpenEXR','OpenImageIO')],proposed)
  item.update(accepted=True,products=len(install['products']),aliases=len(install['aliases']),verified_files=install['verified_files'])
 except ValueError as e:item.update(accepted=False,error=str(e))
 results.append(item);(root/'report.json').write_text(json.dumps(results,indent=2))
 assert item['accepted']==(case=='relocated-positive'),item
print(json.dumps([dict(case=r['case'],accepted=r['accepted'],error=r.get('error')) for r in results]))
