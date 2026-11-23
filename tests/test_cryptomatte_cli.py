"""Public Cryptomatte contract; independent PNG decoding and pinned display pixels."""
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
for name in ('exe','fixture-exe','runtime-dir','schema-dir'):parser.add_argument('--'+name,type=Path,required=True)
args,remaining=parser.parse_known_args()
env=dict(os.environ,OCIO='deliberately-invalid-parent.ocio')
key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH';env[key]=str(args.runtime_dir.resolve())+os.pathsep+env.get(key,'')
validator=Draft202012Validator(json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8')))
jobs_validator=Draft202012Validator(json.loads((args.schema_dir/'jobs-v1.schema.json').read_text(encoding='utf-8')))

def png(path):
    data=Path(path).read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n';position=8;compressed=bytearray();header=None
    while position+12<=len(data):
        count=struct.unpack_from('>I',data,position)[0];tag=data[position+4:position+8];payload=data[position+8:position+8+count]
        assert len(payload)==count and zlib.crc32(tag+payload)==struct.unpack_from('>I',data,position+8+count)[0]
        if tag==b'IHDR':header=struct.unpack('>IIBBBBB',payload)
        if tag==b'IDAT':compressed.extend(payload)
        position+=count+12
        if tag==b'IEND':break
    width,height,depth,kind,compression,filtering,interlace=header
    assert depth==8 and kind in (2,6) and (compression,filtering,interlace)==(0,0,0)
    channels=3 if kind==2 else 4;stride=width*channels;raw=zlib.decompress(compressed);assert len(raw)==height*(stride+1)
    previous=bytearray(stride);pixels=[]
    for y in range(height):
        start=y*(stride+1);mode=raw[start];row=bytearray(raw[start+1:start+1+stride]);assert mode in range(5)
        for x in range(stride):
            a=row[x-channels] if x>=channels else 0;b=previous[x];c=previous[x-channels] if x>=channels else 0
            if mode==1:prediction=a
            elif mode==2:prediction=b
            elif mode==3:prediction=(a+b)//2
            elif mode==4:
                p=a+b-c;distance=(abs(p-a),abs(p-b),abs(p-c));prediction=(a,b,c)[distance.index(min(distance))]
            else:prediction=0
            row[x]=(row[x]+prediction)&255
        pixels.extend(tuple(row[x:x+channels]) for x in range(0,stride,channels));previous=row
    return width,height,pixels

EXPECTED=[(194,72,209),(178,148,123),(194,72,209),(0,0,0),(47,107,44),(83,163,162),(200,47,238),(172,173,94)]

class Cryptomatte(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='thumbgen-crypto-cli-');cls.root=Path(cls.temp.name)
        process=subprocess.run([str(args.fixture_exe),'--fixtures',str(cls.root/'资料 with spaces')],env=env,capture_output=True,text=True,encoding='utf-8',timeout=60)
        assert process.returncode==0,process.stderr
        cls.inputs=json.loads(process.stdout);cls.original={name:hashlib.sha256(Path(path).read_bytes()).hexdigest() for name,path in cls.inputs.items()}
    @classmethod
    def tearDownClass(cls):
        assert cls.original=={name:hashlib.sha256(Path(path).read_bytes()).hexdigest() for name,path in cls.inputs.items()},'source bytes changed'
        cls.temp.cleanup()
    def setUp(self):
        self.directory=Path(tempfile.mkdtemp(dir=self.root));self.output=self.directory/'preview.png'
    def invoke(self,*command,exit=0):
        process=subprocess.run([str(args.exe),*map(str,command),'--json'],env=env,capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(process.returncode,exit,process.stdout+process.stderr)
        rows=[json.loads(line) for line in process.stdout.splitlines() if line.strip()];self.assertTrue(rows)
        for row in rows:validator.validate(row)
        return rows
    def convert(self,source='valid',*options,exit=0):
        return self.invoke('convert',self.inputs[source],'--visualize','cryptomatte','--channels','Object','--output',self.output,*options,exit=exit)[0]
    def batch(self,jobs,exit=3,defaults=None):
        request={'schema_version':1,'execution':{'workers':2,'thread_budget':4,'memory_budget_mib':1024},'jobs':jobs}
        if defaults is not None:request['defaults']=defaults
        jobs_validator.validate(request);path=self.directory/'jobs.json';path.write_text(json.dumps(request),encoding='utf-8')
        rows=self.invoke('batch','--manifest',path,exit=exit);results={row['id']:row for row in rows if row['type']=='result'}
        self.assertEqual(set(results),{job['id'] for job in jobs});self.assertEqual(len(rows),len(jobs)+1)
        return [results[job['id']] for job in jobs]+[rows[-1]]
    def job(self,name,source,**options):
        return {'id':name,'input':self.inputs[source],'sequence_id':'fixed','options':{'visualization':'cryptomatte','channels':'Object','invalid_values':'error','threads':2,**options},'outputs':[{'path':str(self.directory/(name+'.png'))}]}
    def test_capabilities_inspect_and_default_classification(self):
        self.assertIn('cryptomatte',self.invoke('capabilities')[0]['visualization_modes'])
        self.assertIn('auto',self.invoke('capabilities')[0]['visualization_modes'])
        part=self.invoke('inspect',self.inputs['valid'])[0]['parts'][0]
        self.assertEqual(part['cryptomatte_families'],['Object']);self.assertTrue(all(c['kind']=='data' for c in part['candidates']))
        result=self.invoke('convert',self.inputs['valid'],'--output',self.output,exit=4)[0]
        self.assertEqual(result['error']['code'],'visualization_required');self.assertFalse(self.output.exists())
    def test_all_ranks_opaque_ids_signed_weights_and_padding(self):
        result=self.convert();self.assertEqual(png(self.output),(8,1,EXPECTED))
        self.assertEqual(result['cryptomatte']['ranks'],6);self.assertEqual(result['filter'],'triangle')
        self.assertEqual(result['visualization']['range'],[]);self.assertEqual(result['visualization']['invalid_pixels'],0)
        self.assertEqual(result['colour']['config_basis'],'none')
    def test_identity_size_does_not_interpolate_mapped_ids(self):
        self.convert('identity','--size','32768x32768');self.assertEqual(png(self.output),(1280,3,EXPECTED*480))
    def test_half_coverage(self):
        self.convert('half_coverage');self.assertEqual(png(self.output)[2],EXPECTED)
    def test_unused_broken_embedded_manifest(self):
        result=self.convert();self.assertEqual(result['cryptomatte']['manifest'],{'embedded_attribute':True,'sidecar_attribute':False,'used':False})
    def test_sidecar_not_required_or_opened(self):
        result=self.convert('sidecar');self.assertEqual(png(self.output)[2],EXPECTED)
        self.assertEqual(result['cryptomatte']['manifest'],{'embedded_attribute':False,'sidecar_attribute':True,'used':False})
    def test_legacy_metadata_prefix(self):
        result=self.convert('legacy');self.assertEqual(result['cryptomatte']['metadata_key'],'exr/cryptomatte/test123');self.assertEqual(png(self.output)[2],EXPECTED)
    def test_bad_pixels_magenta(self):
        result=self.convert('bad');expected=list(EXPECTED);expected[1]=expected[2]=(255,0,255)
        self.assertEqual(png(self.output)[2],expected);self.assertEqual(result['visualization']['invalid_pixels'],2)
    def test_bad_pixels_black(self):
        result=self.convert('bad','--invalid-values','black');expected=list(EXPECTED);expected[1]=expected[2]=(0,0,0)
        self.assertEqual(png(self.output)[2],expected);self.assertEqual(result['visualization']['invalid_pixels'],2)
    def test_bad_pixels_error_has_no_output(self):
        result=self.convert('bad','--invalid-values','error',exit=4)
        self.assertEqual(result['error']['code'],'data_invalid_value');self.assertEqual(result['visualization']['invalid_pixels'],2);self.assertFalse(self.output.exists())
    def test_existing_skip_and_failed_replace_preserve_output(self):
        self.convert();before=hashlib.sha256(self.output.read_bytes()).hexdigest()
        result=self.convert('bad','--invalid-values','error','--existing','skip')
        self.assertEqual(result['status'],'skipped');self.assertNotIn('visualization',result)
        result=self.convert('bad','--invalid-values','error','--existing','replace',exit=4)
        self.assertEqual(result['error']['code'],'data_invalid_value');self.assertEqual(hashlib.sha256(self.output.read_bytes()).hexdigest(),before)
    def test_invalid_metadata_layout_or_id_precision(self):
        for source,code in [('no_metadata','cryptomatte_metadata_invalid'),('bad_hash','cryptomatte_metadata_invalid'),('gap','cryptomatte_layout_unsupported'),('missing_last','cryptomatte_layout_unsupported'),('half_id','data_type_unsupported')]:
            with self.subTest(source=source):
                result=self.convert(source,exit=4);self.assertEqual(result['error']['code'],code);self.assertFalse(self.output.exists())
    def test_odd_final_pair(self):
        result=self.convert('odd');self.assertEqual(result['cryptomatte']['ranks'],5);self.assertEqual(png(self.output)[2],EXPECTED)
    def test_streamed_layer_decode_limit(self):
        result=self.convert('many','--max-decode-mib','1');self.assertEqual(result['cryptomatte']['ranks'],20)
        self.assertEqual(result['decoded_mib'],0.25);self.assertEqual(set(png(self.output)[2]),{EXPECTED[0]})
    def test_white_and_transparent_background(self):
        self.convert('valid','--background','white');pixels=png(self.output)[2];self.assertEqual(pixels[3],(255,255,255));self.assertEqual(pixels[4],(175,235,171))
        self.output=self.directory/'transparent.png';self.convert('valid','--background','transparent');pixels=png(self.output)[2]
        self.assertEqual(pixels[3],(0,0,0,0));self.assertEqual(pixels[4],(94,214,87,128))
    def test_negative_window_and_orientation(self):
        self.convert('window');width,height,pixels=png(self.output);self.assertEqual((width,height),(6,4))
        self.assertEqual(pixels,[EXPECTED[0] if x in (2,3) and y in (1,2) else (0,0,0) for y in range(4) for x in range(6)])
        self.output=self.directory/'oriented.png';self.convert('oriented');self.assertEqual(png(self.output),(3,2,[EXPECTED[0]]*3+[EXPECTED[7]]*3))
    def test_split_parts_combine_ranks(self):
        result=self.convert('split')
        self.assertEqual(result['cryptomatte']['layers'],['Object00','Object01'])
        self.assertEqual(result['cryptomatte']['ranks'],4)
        self.assertEqual(png(self.output),(2,2,[EXPECTED[1],EXPECTED[3],EXPECTED[3],EXPECTED[3]]))
    def test_explicit_part_must_contain_complete_family(self):
        result=self.convert('split','--part-name','left',exit=4);self.assertEqual(result['error']['code'],'cryptomatte_layout_unsupported');self.assertFalse(self.output.exists())
    def test_auto_batch_matches_colour_and_explicit_cryptomatte(self):
        self.invoke('convert',self.inputs['with_beauty'],'--output',self.output,'--ocio-config','ocio://default')
        colour=self.output.read_bytes()
        self.output=self.directory/'explicit.png'
        self.convert('split')
        crypto=self.output.read_bytes()
        jobs=[]
        for name,source in [('colour','with_beauty'),('auto','split'),('override','split')]:
            job=self.job(name,source)
            del job['sequence_id']
            if name=='colour':job['options']={'ocio_config':'ocio://default'}
            if name=='auto':del job['options']
            jobs.append(job)
        rows=self.batch(jobs,exit=0,defaults={'visualization':'auto'})
        self.assertNotIn('visualization',rows[0])
        self.assertEqual(rows[1]['selection']['basis'],'automatic')
        self.assertEqual(rows[1]['visualization']['mode'],'cryptomatte')
        self.assertEqual(rows[1]['cryptomatte'],rows[2]['cryptomatte'])
        self.assertEqual((self.directory/'colour.png').read_bytes(),colour)
        self.assertEqual((self.directory/'auto.png').read_bytes(),crypto)
        self.assertEqual((self.directory/'override.png').read_bytes(),crypto)
    def test_auto_reports_ambiguity_and_invalid_layout(self):
        for source,code in [('multiple_families','ambiguous_channels'),('gap','cryptomatte_layout_unsupported'),('bad_hash','cryptomatte_metadata_invalid')]:
            with self.subTest(source=source):
                result=self.invoke('convert',self.inputs[source],'--visualize','auto','--output',self.output,exit=4)[0]
                self.assertEqual(result['error']['code'],code)
                self.assertFalse(self.output.exists())
    def test_auto_sequence_preserves_first_preview_kind(self):
        jobs=[self.job(name,source,visualization='auto',channels='') for name,source in
              [('first','split'),('changed','with_beauty'),('recovered','split')]]
        rows=self.batch(jobs)
        self.assertEqual([row['status'] for row in rows[:-1]],['success','failed','success'])
        self.assertEqual(rows[1]['error']['code'],'sequence_selection_changed')
        self.assertEqual(rows[2]['selection']['lock_status'],'reused')
    def test_pool_sequence_error_restore_and_rank_change(self):
        rows=self.batch([self.job('bad','bad'),self.job('good','valid'),self.job('changed','odd'),self.job('recovered','valid')]);results=rows[:-1]
        self.assertEqual([r.get('error',{}).get('code') for r in results],['data_invalid_value',None,'sequence_selection_changed',None])
        self.assertEqual([r['selection']['lock_status'] for r in (results[1],results[3])],['reused','reused'])
        self.assertEqual(png(self.directory/'good.png')[2],EXPECTED);self.assertEqual(png(self.directory/'recovered.png')[2],EXPECTED)
    def test_sequence_settings_are_fixed(self):
        rows=self.batch([self.job('first','valid'),self.job('changed','valid',invalid_values='black'),self.job('last','valid')])
        self.assertEqual(rows[1]['error']['code'],'sequence_selection_conflict');self.assertEqual(rows[2]['status'],'success')
    def test_data_options_and_family_are_explicit(self):
        for options in [('--data-range','0,1'),('--input-space','linear'),('--ocio-config','missing.ocio')]:
            with self.subTest(options=options):self.convert('valid',*options,exit=2)
        result=self.invoke('convert',self.inputs['valid'],'--visualize','cryptomatte','--output',self.output,exit=2)[0];self.assertEqual(result['error']['code'],'invalid_request')
        result=self.invoke('convert',self.inputs['valid'],'--visualize','cryptomatte','--channels','Object00','--output',self.output,exit=4)[0];self.assertEqual(result['error']['code'],'cryptomatte_metadata_invalid')

if __name__=='__main__':unittest.main(argv=[__file__,*remaining])
