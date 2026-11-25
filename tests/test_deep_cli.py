"""Deep colour and automatic opacity previews through the isolated CLI."""
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
from PIL import Image

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
opacity_marker='TG_EXR_AUTO_DEEP_OPACITY selected_part=0 preview=grayscale_coverage'

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
    def invoke(self,*command,exit=0,inherited_ocio=None,text_mode=False):
        command=[str(args.exe),*map(str,command)]
        if not text_mode:command.append('--json')
        process_env=env if inherited_ocio is None else dict(env,OCIO=str(inherited_ocio))
        p=subprocess.run(command,env=process_env,capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(p.returncode,exit,p.stdout+p.stderr)
        self.stderr=p.stderr.splitlines()
        if text_mode:return
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
    def test_positive_infinite_points_with_and_without_back(self):
        for kind in ('infinite_point','infinite_reversed_point','infinite_no_zback'):
            self.output=self.directory/(kind+'.png');self.convert(kind);self.pixels((srgb(.5),0,srgb(.25)))
        self.output=self.directory/'infinite-transparent.png';self.convert('infinite_point','--background','transparent');self.pixels((srgb(2/3),0,srgb(1/3),191))
    def test_infinite_volume_failure_keeps_sequence_identity(self):
        kinds=['scalar','infinite_point','semi_infinite_volume','infinite_reversed_point','scalar']
        jobs=[dict(id=str(i),sequence_id='depth-shot',input=self.inputs[k],outputs=[dict(path=str(self.directory/(str(i)+'.png')))]) for i,k in enumerate(kinds)]
        manifest=dict(schema_version=1,defaults=dict(deep_mode='flatten',ocio_config='ocio://default',input_space='linear',threads=1),jobs=jobs)
        jobs_validator.validate(manifest);path=self.directory/'infinite.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=3);results=[r for r in rows if r['type']=='result']
        self.assertEqual([r['status'] for r in results],['success','success','failed','success','success'])
        self.assertEqual(results[2]['error']['code'],'deep_depth_unsupported')
        self.assertEqual(len({json.dumps(r['selection']['locked'],sort_keys=True) for r in results}),1)
    def test_missing_mapping_and_nonlinear_fail(self):
        self.assertEqual(self.convert('missing_alpha',exit=4)['error']['code'],'deep_channel_mapping_invalid')
        self.assertEqual(self.convert('scalar','--input-space','sRGB',exit=4)['error']['code'],'deep_colour_unsupported')
    def test_auto_deep_marks_only_unresolved_selection(self):
        rejected=self.invoke('convert',self.inputs['scalar'],'--output',self.output,'--visualize','auto',exit=4)[0]
        invalid=self.convert('missing_alpha','--visualize','auto',exit=4)
        for result,code in [(rejected,'deep_requires_flattening'),(invalid,'deep_channel_mapping_invalid')]:
            self.assertEqual(result['error']['code'],code)
            self.assertEqual(result['warnings'],[f'TG_EXR_AUTO_SELECTION_UNRESOLVED error_code={code}'])
        failed=self.convert('scalar','--visualize','auto','--input-space','sRGB',exit=4)
        self.assertEqual(failed['error']['code'],'deep_colour_unsupported')
        self.assertEqual(failed['warnings'],[])
    def auto_opacity(self,kind,*extra,exit=0,**invoke_options):
        rows=self.invoke('convert',self.inputs[kind],'--output',self.output,'--visualize','auto','--threads','1',*extra,exit=exit,**invoke_options)
        if rows:return rows[0]
    def test_auto_opacity_point_order_and_overlapping_volumes(self):
        for kind in ('opacity_points','opacity_ordered','opacity_overlap','opacity_half'):
            with self.subTest(kind=kind):
                self.output=self.directory/(kind+'.png')
                r=self.auto_opacity(kind,inherited_ocio=self.directory/'missing.ocio')
                self.assertEqual(r['warnings'],[opacity_marker])
                self.assertEqual(self.stderr.count(opacity_marker),1)
                self.assertEqual(r['selection']['chosen']['kind'],'deep')
                self.assertEqual(r['selection']['chosen']['group'],'A')
                self.assertEqual(r['deep']['policy'],'deep-opacity-sweep-v1')
                self.assertEqual(r['deep']['mapping'],dict(policy='deep-opacity-mapping-v1',alpha='A',z='Z',zback='ZBack' if kind in ('opacity_overlap','opacity_half') else ''))
                self.assertEqual(r['deep']['samples'],24)
                self.assertEqual(r['colour']['config_basis'],'none')
                # 1 - (1 - 0.5) * (1 - 0.25), directly encoded as coverage.
                self.pixels((159,159,159))
        self.output=self.directory/'overlap-transparent.png'
        self.auto_opacity('opacity_overlap','--background','transparent')
        self.pixels((255,255,255,159))
    def test_auto_opacity_windows_and_backgrounds(self):
        for background in ('black','white','transparent'):
            self.output=self.directory/(background+'.png')
            self.auto_opacity('opacity_window','--background',background,'--ocio-config',self.directory/'unused.ocio')
            w,h,bands,pixels=read_png(self.output)
            self.assertEqual((w,h,bands),(6,5,4 if background=='transparent' else 3))
            for y in range(h):
                for x in range(w):
                    coverage=159 if 1<=x<5 and 1<=y<4 else 0
                    if (x,y)==(1,1):coverage=0
                    if (x,y)==(2,1):coverage=255
                    if background=='transparent':
                        expected=(255,255,255,coverage) if coverage else (0,0,0,0)
                    else:
                        expected=(coverage,)*3
                    self.assertTrue(all(abs(a-b)<=1 for a,b in zip(pixels[y*w+x],expected)))
    def test_auto_opacity_opt_out_layouts_and_colour_precedence(self):
        for kind,flags,code in (
            ('opacity_points',('--deep-mode','reject'),'deep_requires_flattening'),
            ('opacity_points',('--deep-mode','flatten'),'deep_layout_unsupported'),
            ('opacity_points',('--part','0'),'deep_requires_flattening'),
            ('multipart_scalar',('--part-name','coverage'),'deep_requires_flattening'),
            ('opacity_points',('--channels','A'),'deep_requires_flattening'),
            ('opacity_extra',(),'deep_requires_flattening'),
            ('multipart_opacity_points',(),'deep_requires_flattening'),
        ):
            with self.subTest(kind=kind,flags=flags):
                r=self.auto_opacity(kind,*flags,exit=4)
                self.assertEqual(r['error']['code'],code)
                self.assertNotIn(opacity_marker,r['warnings'])
                self.assertFalse(self.output.exists())
        r=self.invoke('convert',self.inputs['opacity_points'],'--output',self.output,exit=4)[0]
        self.assertEqual(r['error']['code'],'deep_requires_flattening')
        r=self.auto_opacity('flat','--input-space','linear')
        self.assertNotIn('deep',r)
        self.assertNotIn(opacity_marker,r['warnings'])
        self.pixels((srgb(.5),0,srgb(.25)))
        self.output=self.directory/'colour.png'
        r=self.convert('multipart_scalar','--visualize','auto')
        self.assertEqual(r['deep']['policy'],'deep-colour-sweep-v1')
        self.assertEqual(r['selection']['chosen']['part'],0)
        self.assertNotIn(opacity_marker,r['warnings'])
        self.pixels((srgb(.5),0,srgb(.25)))
        self.output=self.directory/'coverage.png'
        r=self.auto_opacity('multipart_scalar')
        self.assertEqual(r['selection']['chosen']['part'],1)
        marker=opacity_marker.replace('selected_part=0','selected_part=1')
        self.assertEqual(r['warnings'],[marker])
        self.assertEqual(self.stderr.count(marker),1)
        self.pixels((159,159,159))
    def test_auto_opacity_malformed_samples_and_limits(self):
        for kind,flags,code in (
            ('opacity_points',('--deep-samples-per-pixel','1'),'deep_sample_limit_exceeded'),
            ('opacity_invalid',(),'deep_value_invalid'),
            ('opacity_points',('--memory-budget-mib','512'),'task_memory_exceeds_budget'),
        ):
            with self.subTest(code=code):
                r=self.auto_opacity(kind,*flags,exit=4)
                self.assertEqual(r['error']['code'],code)
                self.assertEqual(r['warnings'],[opacity_marker])
                self.assertEqual(self.stderr.count(opacity_marker),1)
                self.assertFalse(self.output.exists())
        self.auto_opacity('opacity_invalid',exit=4,text_mode=True)
        self.assertEqual(sum('TG_EXR_AUTO_DEEP_OPACITY' in line for line in self.stderr),1)
    def test_auto_opacity_isolated_batch_dual_jpeg_sequence(self):
        kinds=['opacity_points','opacity_ordered','opacity_overlap','opacity_points','opacity_points']
        jobs=[]
        for i,kind in enumerate(kinds):
            outputs=[dict(path=str(self.directory/f'{i}-{size}.jpg'),size=[size,size],quality=95) for size in (4,2)]
            jobs.append(dict(id=str(i),sequence_id='coverage',input=self.inputs[kind],outputs=outputs))
        jobs[3]['options']=dict(deep_mode='reject')
        manifest=dict(schema_version=1,execution=dict(workers=2,thread_budget=2,memory_budget_mib=2048),
            defaults=dict(visualization='auto',threads=1,max_decode_mib=1),jobs=jobs)
        path=self.directory/'batch.json'
        jobs_validator.validate(manifest);path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=3,inherited_ocio=self.directory/'missing.ocio')
        results=sorted((r for r in rows if r['type']=='result'),key=lambda r:r['id'])
        self.assertEqual([r['status'] for r in results],['success','success','failed','failed','success'])
        self.assertEqual(self.stderr.count(opacity_marker),3)
        pin=results[0]['selection']['locked']
        for i in (0,1,4):
            r=results[i]
            self.assertEqual(r['warnings'],[opacity_marker])
            self.assertEqual(r['selection']['lock_status'],'created' if i==0 else 'reused')
            self.assertEqual(r['selection']['locked'],pin)
            for out,size in zip(r['outputs'],((4,3),(2,2))):
                with Image.open(out['path']) as jpeg:
                    self.assertEqual((jpeg.format,jpeg.size),('JPEG',size))
                    self.assertTrue(all(abs(v-159)<=2 for pixel in jpeg.convert('RGB').getdata() for v in pixel))
        for i in (2,3):
            self.assertEqual(results[i]['error']['code'],'sequence_selection_changed')
            self.assertNotIn(opacity_marker,results[i]['warnings'])
            self.assertTrue(all(not Path(out['path']).exists() for out in jobs[i]['outputs']))
        self.assertEqual(results[3]['selection']['locked'],pin)
        self.assertNotIn('chosen',results[3]['selection'])
    def test_auto_opacity_batch_default_reject(self):
        manifest=dict(schema_version=1,defaults=dict(visualization='auto',deep_mode='reject'),
            jobs=[dict(id='reject',input=self.inputs['opacity_points'],outputs=[dict(path=str(self.output))])])
        path=self.directory/'reject.json'
        jobs_validator.validate(manifest);path.write_text(json.dumps(manifest),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=4)
        self.assertEqual(rows[0]['error']['code'],'deep_requires_flattening')
        self.assertNotIn(opacity_marker,rows[0]['warnings'])
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
        for flags in (['--deep-mode',''],['--deep-mode','automatic'],['--deep-samples-per-pixel','0'],['--deep-mode','flatten','--visualize','depth','--channels','Z','--data-range','0,10']):
            self.invoke('convert',self.inputs['scalar'],'--output',self.output,*flags,exit=2)

if __name__=='__main__':unittest.main(argv=[sys.argv[0],*remaining])
