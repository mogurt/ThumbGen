"""Exercise public AOV options, actual PNG pixels and parent/worker contracts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib
from jsonschema import Draft202012Validator

parser=argparse.ArgumentParser()
parser.add_argument('--exe',type=Path,required=True)
parser.add_argument('--aov-exe',type=Path,required=True)
parser.add_argument('--runtime-dir',type=Path,required=True)
parser.add_argument('--schema-dir',type=Path,required=True)
args,remaining=parser.parse_known_args()
environment=dict(os.environ,OCIO='intentionally-invalid-parent-config.ocio')
key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
environment[key]=str(args.runtime_dir.resolve())+os.pathsep+environment.get(key,'')
validator=Draft202012Validator(json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8')))
manifest_validator=Draft202012Validator(json.loads((args.schema_dir/'jobs-v1.schema.json').read_text(encoding='utf-8')))

def png(path):
    data=Path(path).read_bytes()
    assert data[:8]==b'\x89PNG\r\n\x1a\n'
    position=8;compressed=bytearray();header=None
    while position+12<=len(data):
        size=struct.unpack_from('>I',data,position)[0];tag=data[position+4:position+8];payload=data[position+8:position+8+size]
        assert len(payload)==size and zlib.crc32(tag+payload)==struct.unpack_from('>I',data,position+8+size)[0]
        if tag==b'IHDR':header=struct.unpack('>IIBBBBB',payload)
        if tag==b'IDAT':compressed.extend(payload)
        position+=12+size
        if tag==b'IEND':break
    assert header and header[2:]==(8,2,0,0,0),header
    width,height=header[:2];raw=zlib.decompress(compressed);stride=width*3;assert len(raw)==height*(stride+1)
    previous=bytearray(stride);pixels=[]
    for y in range(height):
        start=y*(stride+1);kind=raw[start];row=bytearray(raw[start+1:start+1+stride]);assert kind in range(5)
        for x in range(stride):
            a=row[x-3] if x>=3 else 0;b=previous[x];c=previous[x-3] if x>=3 else 0
            if kind==1:prediction=a
            elif kind==2:prediction=b
            elif kind==3:prediction=(a+b)//2
            elif kind==4:
                p=a+b-c;distances=(abs(p-a),abs(p-b),abs(p-c));prediction=(a,b,c)[distances.index(min(distances))]
            else:prediction=0
            row[x]=(row[x]+prediction)&255
        pixels.extend(tuple(row[x:x+3]) for x in range(0,stride,3));previous=row
    return width,height,pixels

class Aov(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='thumbgen-aov-cli-');cls.root=Path(cls.temp.name)
        process=subprocess.run([str(args.aov_exe),'--fixtures',str(cls.root/'资料 with spaces')],env=environment,capture_output=True,text=True,encoding='utf-8',timeout=30)
        assert process.returncode==0,process.stderr
        cls.inputs=json.loads(process.stdout);cls.original={name:hashlib.sha256(Path(path).read_bytes()).hexdigest() for name,path in cls.inputs.items()}
    @classmethod
    def tearDownClass(cls):
        actual={name:hashlib.sha256(Path(path).read_bytes()).hexdigest() for name,path in cls.inputs.items()}
        assert actual==cls.original,'source bytes changed'
        cls.temp.cleanup()
    def setUp(self):
        self.directory=Path(tempfile.mkdtemp(dir=self.root));self.output=self.directory/'preview.png'
    def invoke(self,*command,exit=0):
        process=subprocess.run([str(args.exe),*map(str,command),'--json'],env=environment,capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(process.returncode,exit,process.stdout+process.stderr)
        records=[json.loads(line) for line in process.stdout.splitlines() if line.strip()]
        self.assertTrue(records)
        for record in records:validator.validate(record)
        return records
    def convert(self,source,mode,channels,*options,exit=0):
        return self.invoke('convert',self.inputs[source],'--output',self.output,'--visualize',mode,'--channels',channels,*options,exit=exit)
    def batch(self,jobs,defaults=None,exit=0):
        request={'schema_version':1,'execution':{'workers':2,'thread_budget':4,'memory_budget_mib':2048},'jobs':jobs}
        if defaults is not None:request['defaults']=defaults
        manifest_validator.validate(request);path=self.directory/'jobs.json';path.write_text(json.dumps(request),encoding='utf-8')
        return self.invoke('batch','--manifest',path,exit=exit)
    def job(self,id,source,options,sequence=None):
        job={'id':id,'input':self.inputs[source],'options':options,'outputs':[{'path':str(self.directory/(id+'.png')),'size':[4,2]}]}
        if sequence is not None:job['sequence_id']=sequence
        return job
    def test_capabilities(self):
        self.assertEqual(self.invoke('capabilities')[0]['visualization_modes'],['auto','colour','depth','normal','vector','id','id_rgb','cryptomatte'])
    def test_depth_native_and_no_ocio(self):
        result=self.convert('depth16.tif','depth','GRAY','--data-range','0,10')[0]
        self.assertEqual(result['colour']['config_basis'],'none');self.assertEqual(result['colour']['basis'],'data_visualization')
        self.assertEqual(result['visualization']['range'],[0,10]);self.assertEqual(png(self.output)[2][:4],[(0,0,0),(128,128,128),(255,255,255),(255,255,255)])
    def test_mixed_normal_and_zero_source_alpha(self):
        self.convert('normal.exr','normal','N');self.assertEqual(set(png(self.output)[2]),{(0,128,255)})
    def test_signed_vector(self):
        self.convert('vector.exr','vector','motion','--data-range','-2,2');self.assertEqual(set(png(self.output)[2]),{(0,255,128)})
    def test_uint32_nearest_id(self):
        result=self.convert('ids.exr','id','object_id','--size','4x2')[0];self.assertEqual(result['filter'],'nearest')
        self.assertEqual(png(self.output)[2][:4],[(119,178,193),(224,142,64),(182,146,193),(213,140,248)])
    def test_double_id_exact(self):
        self.convert('idsdouble.tif','id','GRAY');self.assertEqual(png(self.output)[2][2:4],[(182,146,193),(87,192,129)])
    def test_encoded_rgb_labels(self):
        result=self.convert('labels.exr','id_rgb','matID','--data-range','0,1','--size','2x1')[0]
        self.assertEqual(result['filter'],'nearest');self.assertEqual(png(self.output)[2],[(0,255,0),(128,64,191)])
    def test_encoded_integer_rgb_labels(self):
        self.convert('labels.png','id_rgb','RGB','--data-range','0,255','--size','2x1')
        self.assertEqual(png(self.output)[2],[(0,255,0),(128,64,191)])
    def test_negative_display_window(self):
        self.convert('window.exr','depth','Z','--data-range','0,10')
        self.assertEqual(png(self.output)[2],[(0,0,0),(128,128,128),*[(0,0,0)]*6])
    def test_id_negative_window(self):
        self.convert('idwindow.exr','id','object_id')
        self.assertEqual(png(self.output)[2],[(0,0,0),(213,140,248),*[(0,0,0)]*6])
    def test_data_orientation(self):
        self.convert('oriented.tif','depth','GRAY','--data-range','0,35','--size','4x4')
        width,height,pixels=png(self.output);self.assertEqual((width,height),(2,4))
        self.assertEqual(pixels,[(v,v,v) for v in [146,0,182,36,219,73,255,109]])
    def test_invalid_black(self):
        result=self.convert('idsfloat.exr','id','object_id','--invalid-values','black')[0]
        self.assertEqual(result['visualization']['invalid_pixels'],4);self.assertEqual(png(self.output)[2][2:4],[(0,0,0),(0,0,0)])
    def test_invalid_error_publishes_nothing(self):
        result=self.convert('depth.exr','depth','Z','--data-range','0,10','--invalid-values','error',exit=4)[0]
        self.assertEqual(result['error']['code'],'data_invalid_value');self.assertEqual(result['visualization']['invalid_pixels'],3);self.assertFalse(self.output.exists())
    def test_crypto_metadata_refused(self):
        self.assertEqual(self.convert('crypto.exr','id','Object00.R',exit=4)[0]['error']['code'],'cryptomatte_requires_visualization')
        self.assertFalse(self.output.exists())
    def test_auto_selects_unprefixed_rgb_among_named_aovs_in_either_part_order(self):
        for source,part in [('main-first.exr',0),('main-last.exr',2)]:
            with self.subTest(source=source):
                self.output=self.directory/(source+'.png')
                result=self.invoke('convert',self.inputs[source],'--visualize','auto','--ocio-config','ocio://default',
                                   '--output',self.output)[0]
                chosen=result['selection']['chosen']
                self.assertEqual((chosen['part'],chosen['part_name'],chosen['group']),(part,'main','RGB'))
                self.assertEqual(chosen['channels'],['R','G','B','A'])
                self.assertIn('unprefixed RGB',chosen['reason'])
                self.assertNotIn('visualization',result)
        self.assertEqual((self.directory/'main-first.exr.png').read_bytes(),self.output.read_bytes())
    def test_auto_keeps_single_part_root_rgb_with_named_light_groups(self):
        result=self.invoke('convert',self.inputs['root-with-light-groups.exr'],'--visualize','auto','--ocio-config','ocio://default',
                           '--output',self.output)[0]
        selection=result['selection']
        self.assertEqual({c['group'] for c in selection['candidates']},{'RGB','diffuse_direct'})
        self.assertEqual(selection['chosen']['group'],'RGB')
        self.assertEqual(selection['chosen']['channels'],['R','G','B'])
    def test_auto_rejects_other_colour_views_and_prefixed_rgb(self):
        for source,code in [('two-views.exr','ambiguous_channels'),('other-view.exr','ambiguous_parts'),
                            ('two-roots.exr','ambiguous_parts'),('prefixed-rgb.exr','ambiguous_parts')]:
            with self.subTest(source=source):
                result=self.invoke('convert',self.inputs[source],'--visualize','auto','--output',self.output,exit=4)[0]
                self.assertEqual(result['error']['code'],code)
                self.assertFalse(self.output.exists())
    def test_auto_keeps_primary_and_explicit_selection_priorities(self):
        colour=self.invoke('convert',self.inputs['main-first.exr'],'--output',self.output,exit=4)[0]
        self.assertEqual(colour['error']['code'],'ambiguous_parts')
        for source,options,part,group in [
            ('root-with-light-groups.exr',['--selection-policy','root_rgb'],'main','RGB'),
            ('root-with-light-groups.exr',['--part','0'],'main','RGB'),
            ('main-first.exr',['--part-name','sHaDoW'],'sHaDoW','RGB'),
            ('main-first.exr',['--channels','Fresnel'],'passes','Fresnel'),
            ('root-with-primary.exr',[],'main','Beauty'),
            ('named-primary.exr',[],'Beauty','RGB'),
        ]:
            with self.subTest(source=source,options=options):
                result=self.invoke('convert',self.inputs[source],'--visualize','auto','--ocio-config','ocio://default',
                                   '--output',self.output,*options)[0]
                chosen=result['selection']['chosen']
                self.assertEqual((chosen['part_name'],chosen['group']),(part,group))
                self.assertEqual(chosen['primary'],part=='Beauty' or group=='Beauty')
                self.output.unlink()
    def test_auto_sequence_reuses_a_named_part_across_reordering_and_new_primary(self):
        jobs=[self.job(name,name+'.exr',{'visualization':'auto','ocio_config':'ocio://default'},'main')
              for name in ['main-first','main-last','named-primary']]
        results={r['id']:r['selection'] for r in self.batch(jobs) if r['type']=='result'}
        self.assertEqual(results['main-first']['lock_status'],'created')
        for name,part in [('main-last',2),('named-primary',0)]:
            chosen=results[name]['chosen']
            self.assertEqual((chosen['part'],chosen['part_name']),(part,'main'))
            self.assertEqual(results[name]['lock_status'],'reused')
    def test_required_and_incompatible_options(self):
        cases=[('depth','Z',[]),('vector','motion',[]),('id_rgb','matID',[]),('id','object_id',['--data-range','0,1']),
               ('normal','N',['--data-range','1,1']),('normal','N',['--data-range','0,nan']),
               ('normal','N',['--input-space','linear']),('normal','N',['--ocio-config','other.ocio'])]
        for mode,channels,options in cases:
            with self.subTest(mode=mode,options=options):self.convert('normal.exr',mode,channels,*options,exit=2)
        self.invoke('convert',self.inputs['normal.exr'],'--output',self.output,'--visualize','normal',exit=2)
        self.assertFalse(self.output.exists())
    def test_pool_sequence_lock_and_recovery(self):
        options={'visualization':'depth','channels':'Z','data_range':[0,10],'invalid_values':'error'}
        jobs=[self.job('bad','depth.exr',options,'depth'),self.job('good','depthgood.exr',options,'depth'),
              self.job('range','depthgood.exr',dict(options,data_range=[0,20]),'depth'),self.job('recovered','depthgood.exr',options,'depth'),
              self.job('invalid','depthgood.exr',dict(options,invalid_values='black'),'depth'),
              self.job('mode','depthgood.exr',dict(options,visualization='id',data_range=[]),'depth')]
        records=self.batch(jobs,exit=3);by_id={r['id']:r for r in records if r['type']=='result'}
        self.assertEqual(by_id['bad']['error']['code'],'data_invalid_value')
        for id in ['range','invalid','mode']:self.assertEqual(by_id[id]['error']['code'],'sequence_selection_conflict')
        for id in ['good','recovered']:self.assertTrue(by_id[id]['ok']);self.assertEqual(by_id[id]['selection']['lock_status'],'reused')
        self.assertEqual(records[-1]['success'],2);self.assertEqual(records[-1]['failed'],4)
    def test_inherited_range_reset_and_normal_equivalence(self):
        defaults={'visualization':'normal','channels':'N','data_range':[-1,1]}
        jobs=[self.job('normal','normal.exr',{},'normal'),self.job('normal2','normal.exr',{'data_range':[]},'normal'),
              self.job('id','ids.exr',{'visualization':'id','channels':'object_id','data_range':[]})]
        records=self.batch(jobs,defaults);self.assertEqual(records[-1]['success'],3)
        self.assertEqual([r for r in records if r.get('id')=='normal2'][0]['selection']['lock_status'],'reused')

if __name__=='__main__':unittest.main(argv=[__file__,*remaining])
