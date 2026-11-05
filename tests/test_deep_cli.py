"""Public opt-in Deep conversion: numeric pixels, stable locks and admission."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from jsonschema import Draft202012Validator

sys.path.insert(0,str(Path(__file__).parent/'qa'))
from alpha_reference import read_png

parser=argparse.ArgumentParser()
parser.add_argument('--exe',required=True,type=Path)
parser.add_argument('--fixtures-exe',required=True,type=Path)
parser.add_argument('--runtime-dir',required=True,type=Path)
parser.add_argument('--schema-dir',required=True,type=Path)
args,remaining=parser.parse_known_args()
env=dict(os.environ);env.pop('OCIO',None)
key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
env[key]=str(args.runtime_dir.resolve())+os.pathsep+env.get(key,'')
validator=Draft202012Validator(json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8')))
jobs_validator=Draft202012Validator(json.loads((args.schema_dir/'jobs-v1.schema.json').read_text(encoding='utf-8')))

def srgb(value):
    return round(255*(12.92*value if value<=.0031308 else 1.055*value**(1/2.4)-.055))

class DeepCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='thumbgen-deep-cli-');cls.root=Path(cls.temp.name)
        p=subprocess.run([str(args.fixtures_exe),str(cls.root/'inputs')],env=env,capture_output=True,text=True,encoding='utf-8',timeout=30)
        assert p.returncode==0,p.stderr
        cls.inputs=json.loads(p.stdout)
        cls.hashes={k:hashlib.sha256(Path(v).read_bytes()).hexdigest() for k,v in cls.inputs.items()}
    @classmethod
    def tearDownClass(cls):
        assert cls.hashes=={k:hashlib.sha256(Path(v).read_bytes()).hexdigest() for k,v in cls.inputs.items()}
        cls.temp.cleanup()
    def setUp(self):
        self.directory=Path(tempfile.mkdtemp(dir=self.root));self.output=self.directory/'image.png'
    def invoke(self,*command,exit=0):
        p=subprocess.run([str(args.exe),*map(str,command),'--json'],env=env,capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(p.returncode,exit,p.stdout+p.stderr)
        rows=[json.loads(s) for s in p.stdout.splitlines() if s.strip()]
        for row in rows:validator.validate(row)
        return rows
    def convert(self,kind='scalar',*extra,exit=0):
        rows=self.invoke('convert',self.inputs[kind],'--output',self.output,'--deep-mode','flatten','--threads','1','--ocio-config','ocio://default','--input-space','linear',*extra,exit=exit)
        return rows[0]
    def pixels(self,expected):
        w,h,bands,pixels=read_png(self.output)
        self.assertEqual((w,h,bands),(4,3,len(expected)))
        for pixel in pixels:
            self.assertTrue(all(abs(a-b)<=1 for a,b in zip(pixel,expected)),(pixel,expected))
    def test_default_reject_and_capabilities(self):
        rows=self.invoke('convert',self.inputs['scalar'],'--output',self.output,exit=4)
        self.assertEqual(rows[0]['error']['code'],'deep_requires_flattening')
        self.assertFalse(self.output.exists())
        self.assertEqual(self.invoke('capabilities')[0]['deep_modes'],['reject','flatten'])
    def test_black_background_scan_tile_and_volume(self):
        for kind in ('scalar','tiled','volumes'):
            self.output=self.directory/(kind+'.png');r=self.convert(kind)
            self.assertEqual(r['deep']['samples'],24)
            self.assertGreater(r['deep']['peak_buffer_bytes'],0)
            self.pixels((srgb(.5),0,srgb(.25)))
    def test_white_and_coloured_transmission(self):
        self.convert('scalar','--background','white');self.pixels((srgb(.75),srgb(.25),srgb(.5)))
        self.output=self.directory/'coloured.png';self.convert('coloured','--background','white')
        self.pixels((srgb(.75),srgb(.5625),srgb(.1875)))
    def test_scalar_transparent_and_empty(self):
        self.convert('scalar','--background','transparent');self.pixels((srgb(2/3),0,srgb(1/3),191))
        self.output=self.directory/'empty.png';self.convert('empty','--background','transparent');self.pixels((0,0,0,0))
    def test_transparency_refusals_and_emission_on_black(self):
        r=self.convert('coloured','--background','transparent',exit=4)
        self.assertEqual(r['error']['code'],'deep_transparency_unsupported')
        r=self.convert('emission','--background','transparent',exit=4)
        self.assertEqual(r['error']['code'],'deep_transparency_unsupported')
        self.convert('emission');self.pixels((255,0,0))
    def test_primary_stays_beauty_despite_black_root(self):
        r=self.convert('beauty');self.assertEqual(r['selection']['chosen']['group'],'beauty');self.pixels((srgb(.5),0,srgb(.25)))
        self.output=self.directory/'root.png';self.convert('beauty','--selection-policy','root_rgb');self.pixels((0,0,0))
    def test_flat_input_keeps_ordinary_path(self):
        r=self.convert('flat');self.assertNotIn('deep',r);self.pixels((srgb(.5),0,srgb(.25)))
    def test_layer_auxiliary_depth_cannot_override_base(self):
        r=self.convert('depth_collision')
        self.assertEqual(r['deep']['mapping']['z'],'Z');self.assertEqual(r['deep']['mapping']['zback'],'Zback')
        self.assertEqual(r['deep']['mapping']['policy'],'deep-mapping-v2')
        self.pixels((srgb(.5),0,srgb(.25)))
    def test_enclosing_alpha_and_partial_triple(self):
        r=self.convert('ancestor_alpha','--background','white')
        self.assertEqual(r['deep']['mapping']['alpha'],['view.A']*3);self.pixels((srgb(.875),srgb(.8125),srgb(.9375)))
        self.output=self.directory/'mixed.png';r=self.convert('mixed_alpha')
        self.assertEqual(r['deep']['mapping']['alpha'],['AR','A','A']);self.pixels((srgb(.25),srgb(.5),srgb(.5)))
        self.output=self.directory/'mixed-white.png';self.convert('mixed_alpha','--background','white');self.pixels((255,255,255))
        self.output=self.directory/'mixed-transparent.png'
        self.assertEqual(self.convert('mixed_alpha','--background','transparent',exit=4)['error']['code'],'deep_transparency_unsupported')
    def test_luminance_uses_scalar_alpha(self):
        r=self.convert('grey_alpha','--background','transparent')
        self.assertEqual(r['deep']['mapping']['alpha'],['A']*3);self.pixels((srgb(.5),srgb(.5),srgb(.5),128))
    def test_ancestor_alpha_disappearance_keeps_original_lock(self):
        kinds=['ancestor_alpha','ancestor_alpha_shift','ancestor_alpha']
        jobs=[dict(id=str(i),sequence_id='ancestor-shot',input=self.inputs[k],outputs=[dict(path=str(self.directory/(str(i)+'.png')))]) for i,k in enumerate(kinds)]
        manifest=dict(schema_version=1,defaults=dict(deep_mode='flatten',ocio_config='ocio://default',input_space='linear',threads=1),jobs=jobs)
        jobs_validator.validate(manifest);path=self.directory/'ancestor.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=3);results=[r for r in rows if r['type']=='result']
        self.assertEqual([r['status'] for r in results],['success','failed','success'])
        self.assertEqual(results[1]['error']['code'],'sequence_selection_changed')
        self.assertEqual(results[2]['selection']['locked']['deep']['alpha'],['view.A']*3)
    def test_missing_mapping_and_nonlinear_fail(self):
        self.assertEqual(self.convert('missing_alpha',exit=4)['error']['code'],'deep_channel_mapping_invalid')
        self.assertEqual(self.convert('scalar','--input-space','sRGB',exit=4)['error']['code'],'deep_colour_unsupported')
    def test_sample_limit_and_parent_admission(self):
        self.assertEqual(self.convert('scalar','--deep-samples-per-pixel','1',exit=4)['error']['code'],'deep_sample_limit_exceeded')
        r=self.convert('scalar','--memory-budget-mib','512',exit=4)
        self.assertEqual(r['error']['code'],'task_memory_exceeds_budget');self.assertFalse(self.output.exists())
        self.convert('scalar','--max-decode-mib','1','--memory-budget-mib','512');self.pixels((srgb(.5),0,srgb(.25)))
    def test_sequence_identity_reorder_and_no_fallback(self):
        kinds=['scalar','empty','changed_alpha','reordered','no_zback','scalar']
        jobs=[dict(id=str(i),sequence_id='shot',input=self.inputs[k],outputs=[dict(path=str(self.directory/(str(i)+'.png')))]) for i,k in enumerate(kinds)]
        manifest=dict(schema_version=1,defaults=dict(deep_mode='flatten',ocio_config='ocio://default',input_space='linear',threads=1),jobs=jobs)
        jobs_validator.validate(manifest);path=self.directory/'jobs.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=3);results=[r for r in rows if r['type']=='result']
        self.assertEqual([r['status'] for r in results],['success','success','failed','success','failed','success'])
        for i in (2,4):self.assertEqual(results[i]['error']['code'],'sequence_selection_changed')
        for i in (1,3,5):self.assertEqual(results[i]['selection']['lock_status'],'reused')
    def test_parallel_workers_restore_same_deep_pin(self):
        jobs=[dict(id=str(i),sequence_id='parallel-shot',input=self.inputs['scalar' if i%2==0 else 'reordered'],outputs=[dict(path=str(self.directory/(str(i)+'.png')))]) for i in range(8)]
        manifest=dict(schema_version=1,execution=dict(workers=2,thread_budget=2,memory_budget_mib=2048),
            defaults=dict(deep_mode='flatten',ocio_config='ocio://default',input_space='linear',threads=1),jobs=jobs)
        jobs_validator.validate(manifest);path=self.directory/'parallel.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path);results=[r for r in rows if r['type']=='result']
        self.assertEqual(len(results),8)
        self.assertTrue(all(r['status']=='success' for r in results))
        self.assertEqual(len({json.dumps(r['selection']['locked'],sort_keys=True) for r in results}),1)
        pixels=[read_png(Path(job['outputs'][0]['path'])) for job in jobs]
        self.assertTrue(all(p==pixels[0] for p in pixels))
    def test_window_padding(self):
        self.convert('window','--background','white');w,h,bands,pixels=read_png(self.output)
        self.assertEqual((w,h,bands),(6,5,3))
        for y in range(h):
            for x in range(w):
                expected=(srgb(.75),srgb(.25),srgb(.5)) if 1<=x<5 and 1<=y<4 else (255,255,255)
                self.assertTrue(all(abs(a-b)<=1 for a,b in zip(pixels[y*w+x],expected)))
    def test_invalid_options(self):
        for flags in (['--deep-mode','automatic'],['--deep-samples-per-pixel','0'],['--deep-mode','flatten','--visualize','depth','--channels','Z','--data-range','0,10']):
            self.invoke('convert',self.inputs['scalar'],'--output',self.output,*flags,exit=2)

if __name__=='__main__':unittest.main(argv=[sys.argv[0],*remaining])
