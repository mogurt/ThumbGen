"""Integer colour plan through the public worker pool, including failure recovery."""
import argparse,json,os,struct,subprocess,tempfile,zlib
from pathlib import Path
import jsonschema
p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--runtime-dir',type=Path,required=True);p.add_argument('--schema-dir',type=Path,required=True);a=p.parse_args()
env=dict(os.environ);env.pop('OCIO',None);key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH';env[key]=str(a.runtime_dir.resolve())+os.pathsep+env.get(key,'')
validator=jsonschema.Draft202012Validator(json.loads((a.schema_dir/'protocol-v1.schema.json').read_text()))
with tempfile.TemporaryDirectory(prefix='integer-cli-') as tmp:
 root=Path(tmp);source=root/'source.png'
 def chunk(k,b):return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b)&0xffffffff)
 # RGBA16: meaningful alpha including zero; one complete source for two sizes.
 scan=b''.join(b'\0'+b''.join(struct.pack('>HHHH',(x*977+y*353)%65536,x*257%65536,y*503%65536,x*521%65536) for x in range(127)) for y in range(65))
 source.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',127,65,16,6,0,0,0))+chunk(b'sRGB',b'\0')+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
 jobs=[]
 for i in range(12):
  jobs.append(dict(id=str(i),input=str(source),options=dict(background='transparent'),outputs=[dict(path=str(root/f'{i}-{w}.png'),size=[w,w],format='png') for w in (32,13)]))
 # Force a decode admission failure between successful tasks in the same pool.
 jobs.insert(3,dict(id='limited',input=str(source),options=dict(max_decode_mib=1),outputs=[dict(path=str(root/'limited.png'),format='png')]))
 # A huge image header is refused before decode; following tasks must still run.
 big=root/'big.png';big.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8192,8192,16,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'))+chunk(b'IEND',b''))
 jobs[3]['input']=str(big)
 manifest=dict(schema_version=1,execution=dict(workers=4,thread_budget=4,memory_budget_mib=1024),defaults=dict(threads=1,input_space='sRGB'),jobs=jobs)
 path=root/'jobs.json';path.write_text(json.dumps(manifest))
 run=subprocess.run([str(a.exe.resolve()),'batch','--manifest',str(path),'--json'],env=env,capture_output=True,text=True,timeout=90)
 rows=[json.loads(line) for line in run.stdout.splitlines()];assert rows,(run.returncode,run.stderr)
 for row in rows:validator.validate(row)
 results={r['id']:r for r in rows if r['type']=='result'};assert len(results)==13,run.stdout
 assert results['limited']['error']['code'] in ('task_memory_exceeds_budget','decode_limit_exceeded'),results['limited']
 for i in range(12):
  row=results[str(i)];assert row['ok'] and row['colour']['processing']=='integer-linear-v1',row
  assert len(row['outputs'])==2 and all(Path(o['path']).is_file() for o in row['outputs']),row
 assert not list(root.glob('*.tmp*'))
 print('4 workers: 12 dual-output successes, one predecode budget rejection, protocol valid, pool recovered')
