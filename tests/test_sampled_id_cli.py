"""Public sampled-ID contract, independent integer palette and binary32 geometry."""
import argparse
import ast
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib
from jsonschema import Draft202012Validator

parser=argparse.ArgumentParser()
for name in ('exe','fixture-exe','runtime-dir','schema-dir','png-reference'):parser.add_argument('--'+name,type=Path,required=True)
args,remaining=parser.parse_known_args()
env=dict(os.environ,OCIO='invalid-parent-config.ocio');key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
env[key]=str(args.runtime_dir.resolve())+os.pathsep+env.get(key,'')
schema=Draft202012Validator(json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8')))
tree=ast.parse(args.png_reference.read_text(encoding='utf-8'))
function=next(node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='png')
scope=dict(Path=Path,struct=struct,zlib=zlib);exec(compile(ast.Module(body=[function],type_ignores=[]),'png_reference','exec'),scope);png=scope['png']

def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
def expected(width,height,w,h):
    labels=(0,1,42,16777216,16777217,4294967295)
    xs=[int(math.floor(f32(f32(f32(x+0.5)*f32(1/w))*width))) for x in range(w)]
    ys=[int(math.floor(f32(f32(f32(y+0.5)*f32(1/h))*height))) for y in range(h)]
    result=[]
    for y in ys:
        for x in xs:
            identity=labels[(x//17+y//13)%6];hashed=(identity*2654435761)&0xffffffff
            hashed^=((hashed<<13)|(hashed>>19))&0xffffffff;hashed^=hashed>>17
            result.append(tuple(64+((hashed>>(c*8))%192) if identity else 0 for c in range(3)))
    return result

class SampledId(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='thumbgen-sampled-cli-');cls.root=Path(cls.temp.name)
        cls.inputs={}
        for name,w,h in [('sparse',4099,2053),('dense',65,33),('wide',16384,1024)]:
            path=cls.root/(name+'.exr');subprocess.run([str(args.fixture_exe),'--fixture',str(path),str(w),str(h)],env=env,capture_output=True,check=True,timeout=60)
            cls.inputs[name]=path
        cls.hashes={k:hashlib.sha256(p.read_bytes()).hexdigest() for k,p in cls.inputs.items()}
    @classmethod
    def tearDownClass(cls):
        assert cls.hashes=={k:hashlib.sha256(p.read_bytes()).hexdigest() for k,p in cls.inputs.items()}
        cls.temp.cleanup()
    def setUp(self):self.directory=Path(tempfile.mkdtemp(dir=self.root));self.output=self.directory/'preview.png'
    def invoke(self,*options,exit=0):
        process=subprocess.run([str(args.exe),*map(str,options),'--json'],env=env,capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(process.returncode,exit,process.stdout+process.stderr)
        rows=[json.loads(line) for line in process.stdout.splitlines() if line.strip()]
        for row in rows:schema.validate(row)
        self.assertTrue(rows);return rows
    def convert(self,name='sparse',*options,exit=0):
        return self.invoke('convert',self.inputs[name],'--visualize','id','--channels','object_id','--output',self.output,*options,exit=exit)[0]
    def assert_pixels(self,path,width,height):
        w,h,pixels=png(path);self.assertEqual(pixels,expected(width,height,w,h));return w,h
    def test_low_decode_limit_with_exact_integer_pixels(self):
        row=self.convert('sparse','--size',31,'--max-decode-mib',1)
        self.assertEqual(row['source_read']['mode'],'sampled_blocks');self.assertLess(row['decoded_mib'],1)
        self.assertEqual(row['decoded_mib']*1048576,row['source_read']['buffer_bytes'])
        self.assertEqual(self.assert_pixels(self.output,4099,2053),(31,16));self.assertEqual(row['visualization']['invalid_pixels'],0)
    def test_dense_preview_retains_full_source(self):
        row=self.convert('dense','--size',65)
        self.assertNotIn('source_read',row);self.assertEqual(row['decoded_mib']*1048576,65*33*4)
        self.assertEqual(self.assert_pixels(self.output,65,33),(65,33))
    def test_sampled_read_does_not_depend_on_invalid_policy(self):
        self.convert('sparse','--size',31,'--invalid-values','error','--max-decode-mib',1)
        self.assert_pixels(self.output,4099,2053)
    def test_sampled_source_failure_preserves_existing_target(self):
        self.output.write_bytes(b'preserve-old');before=self.output.read_bytes()
        row=self.convert('wide','--size',1,'--max-decode-mib',1,'--existing','replace',exit=4)
        self.assertEqual(row['error']['code'],'decode_limit_exceeded');self.assertEqual(self.output.read_bytes(),before)
    def test_all_skip_remains_metadata_only(self):
        self.output.write_bytes(b'preserve-old')
        row=self.convert('wide','--size',1,'--max-decode-mib',1,'--existing','skip')
        self.assertEqual(row['status'],'skipped');self.assertNotIn('source_read',row);self.assertEqual(self.output.read_bytes(),b'preserve-old')
    def test_multi_size_union_and_repeated_encoder_sizes(self):
        outputs=[dict(path=str(self.directory/f'{i}.png'),size=[edge,edge]) for i,edge in enumerate([31,13,31])]
        request=dict(schema_version=1,execution=dict(workers=2,thread_budget=4,memory_budget_mib=512),jobs=[dict(id='multi',input=str(self.inputs['sparse']),options=dict(visualization='id',channels='object_id',max_decode_mib=1),outputs=outputs)])
        path=self.directory/'jobs.json';path.write_text(json.dumps(request),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path);self.assertEqual(rows[-1]['success'],1)
        self.assertEqual(rows[0]['source_read']['mode'],'sampled_blocks')
        for output in outputs:self.assert_pixels(output['path'],4099,2053)
        self.assertEqual(Path(outputs[0]['path']).read_bytes(),Path(outputs[2]['path']).read_bytes())
    def test_sequence_lock_is_stable_across_sampled_and_full_paths(self):
        jobs=[dict(id=str(i),sequence_id='fixed-id',input=str(self.inputs['sparse']),options=dict(visualization='id',channels='object_id'),outputs=[dict(path=str(self.directory/f'{i}.png'),size=[edge,edge])]) for i,edge in enumerate([31,1024,13])]
        path=self.directory/'sequence.json';path.write_text(json.dumps(dict(schema_version=1,execution=dict(workers=2,thread_budget=4,memory_budget_mib=1024),jobs=jobs)),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path);by_id={row['id']:row for row in rows if row['type']=='result'}
        self.assertIn('source_read',by_id['0']);self.assertNotIn('source_read',by_id['1']);self.assertIn('source_read',by_id['2'])
        self.assertEqual(by_id['0']['selection']['locked'],by_id['1']['selection']['locked']);self.assertEqual(by_id['0']['selection']['locked'],by_id['2']['selection']['locked'])
        for i in ('0','1','2'):self.assert_pixels(self.directory/(i+'.png'),4099,2053)

if __name__=='__main__':unittest.main(argv=[__file__,*remaining])
