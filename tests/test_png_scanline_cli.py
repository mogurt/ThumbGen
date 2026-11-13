"""Synthetic C1 paired functional checks. Supply an isolated work directory."""
from pathlib import Path
import subprocess,os,struct,zlib,json
import argparse
p=argparse.ArgumentParser(description='Pair synthetic 8MP PNG16 and Adam7/error cases against a frozen executable; no timing claims')
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--candidate',type=Path,required=True)
p.add_argument('--workdir',type=Path,required=True)
a=p.parse_args()
root=a.workdir;root.mkdir(parents=True,exist_ok=True)
base=a.baseline.resolve();exe=a.candidate.resolve();env=dict(os.environ,OCIO='ocio://default')
def chunk(k,b):return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b)&0xffffffff)
def fixture(nc,interlace=0):
 w,h=4001,2000
 row=b''.join(struct.pack('>'+'H'*nc,*([(x*977)%65536,(x*353)%65536,(x*7919)%65536]+([(x*521)%65536] if nc==4 else []))) for x in range(w))
 compressor=zlib.compressobj(1);parts=[]
 passes=[(0,0,1,1)] if not interlace else [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
 for x0,y0,dx,dy in passes:
  scan=b'\0'+b''.join(row[x*nc*2:(x+1)*nc*2] for x in range(x0,w,dx))
  for y in range(y0,h,dy):parts.append(compressor.compress(scan))
 parts.append(compressor.flush())
 prefix=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,16,6 if nc==4 else 2,0,0,interlace))+chunk(b'sRGB',b'\0')+chunk(b'tEXt',b'Description\0C1 synthetic fixture')
 data=b''.join(parts);path=root/f'source-{nc}-{interlace}.dat';path.write_bytes(prefix+chunk(b'IDAT',data)+chunk(b'IEND',b''))
 bad=root/f'bad-{nc}.png';bad.write_bytes(prefix+chunk(b'IDAT',data[:len(data)//2])+chunk(b'IEND',b''))
 return path,bad
records=[]
for nc,interlace in [(3,0),(4,0),(4,1)]:
 src,bad=fixture(nc,interlace)
 for label,source in [('ok',src),('bad',bad)]:
  rows=[]
  for name,program in [('d1',base),('c1',exe)]:
   dest=root/f'{nc}-{interlace}-{label}-{name}.png'
   run=subprocess.run([str(program),'convert',str(source.resolve()),'--output',str(dest.resolve()),'--size','425x260','--threads','1','--background','transparent','--existing','replace','--json'],capture_output=True,text=True,env=env,timeout=90)
   row=next(json.loads(line) for line in run.stdout.splitlines() if json.loads(line).get('type')=='result');rows.append(row)
  assert rows[0]['ok']==rows[1]['ok'],rows
  if rows[0]['ok']:
   assert Path(rows[0]['outputs'][0]['path']).read_bytes()==Path(rows[1]['outputs'][0]['path']).read_bytes(),rows
   assert rows[1]['colour']['processing']=='integer-linear-v1',rows
  else:assert rows[0]['error']['code']==rows[1]['error']['code'],rows
  records.append(dict(channels=nc,adam7=interlace,case=label,ok=rows[1]['ok'],exact_png=rows[1]['ok'],error=rows[1].get('error')))
(root/'results.json').write_text(json.dumps(records,indent=2));print(json.dumps(records,indent=2))
