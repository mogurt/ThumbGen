"""Validate actual CLI and native classification records against the published schemas."""
import argparse
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from jsonschema import Draft202012Validator, ValidationError

parser=argparse.ArgumentParser()
parser.add_argument('--exe',required=True,type=Path)
parser.add_argument('--matrix-exe',required=True,type=Path)
parser.add_argument('--schema-dir',required=True,type=Path)
parser.add_argument('--runtime-dir',type=Path)
args,remaining=parser.parse_known_args()
ENV=dict(os.environ,OCIO='ocio://default')
if args.runtime_dir:
    key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
    ENV[key]=str(args.runtime_dir.resolve())+os.pathsep+ENV.get(key,'')
schema=json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8'))
validator=Draft202012Validator(schema)

def png(path):
    def chunk(kind,payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,2,0,0,0))+
                     chunk(b'IDAT',zlib.compress(b'\0\xff\0\0\xff\0\0'))+chunk(b'IEND',b''))

class Protocol(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='thumbgen-protocol-')
        self.root=Path(self.temp.name);self.input=self.root/'source.png';png(self.input)
    def tearDown(self):self.temp.cleanup()
    def records(self,*command,exit=0,stdin=None):
        process=subprocess.run([str(args.exe),*map(str,command),'--json'],env=ENV,input=stdin,
                               capture_output=True,text=True,encoding='utf-8',timeout=30)
        self.assertEqual(process.returncode,exit,(process.stdout,process.stderr))
        rows=[json.loads(line) for line in process.stdout.splitlines()]
        self.assertTrue(rows)
        for row in rows:validator.validate(row)
        if rows[-1]['type']=='summary':
            end=rows[-1];results=rows[:-1]
            self.assertEqual(end['total'],len(results))
            for state,key in [('success','success'),('partial','partial'),('failed','failed')]:
                self.assertEqual(end[key],sum(r['status']==state for r in results))
            outputs=[out for r in results for out in r['outputs']]
            self.assertEqual(end['output_counts']['success'],sum(o['status']=='success' for o in outputs))
            self.assertEqual(end['output_counts']['failed'],sum(o['status']=='failed' for o in outputs))
            self.assertEqual(end['output_counts']['total'],sum(end['output_counts'][k] for k in ['success','failed','not_attempted']))
        return rows
    def test_schema_and_registry(self):
        Draft202012Validator.check_schema(schema)
        manifest=json.loads((args.schema_dir/'jobs-v1.schema.json').read_text(encoding='utf-8'))
        Draft202012Validator.check_schema(manifest)
        registry=json.loads((args.schema_dir/'errors-v1.json').read_text(encoding='utf-8'))['errors']
        codes=[r['code'] for r in registry]
        self.assertEqual(len(codes),len(set(codes)))
        self.assertEqual(codes,schema['$defs']['error']['properties']['code']['enum'])
    def test_public_record_types(self):
        self.records('convert',self.input,'--output',self.root/'success.png')
        self.records('inspect',self.input)
        self.records('inspect',self.root/'missing.exr',exit=4)
        version=self.records('--version')[0]
        caps=self.records('capabilities')[0]
        self.assertEqual(version['build'],caps['build'])
        self.assertTrue(caps['build']['revision']);self.assertTrue(caps['build']['compiler'])
        self.assertIn('jpeg',caps['input_plugins']);self.assertIn('png',caps['output_formats'])
        self.records('unknown-command',exit=2)
        self.records('convert',self.root/'missing.exr','--output',self.root/'missing.png',exit=4)
    def test_batch_counts_and_partial_outputs(self):
        existing=self.root/'exists.png';existing.write_bytes(b'keep')
        jobs=[{'id':'ok','input':str(self.input),'outputs':[{'path':str(self.root/'ok.png')}]},
              {'id':'partial','input':str(self.input),'outputs':[{'path':str(existing)},{'path':str(self.root/'new.png')}]},
              {'id':'bad','input':str(self.root/'missing.exr'),'outputs':[{'path':str(self.root/'bad.png')}]}]
        path=self.root/'jobs.json';path.write_text(json.dumps({'schema_version':1,'jobs':jobs}))
        rows=self.records('batch','--manifest',path,exit=3)
        self.assertEqual(rows[-1]['output_counts'],{'total':4,'success':2,'failed':1,'not_attempted':1})
        self.assertEqual(existing.read_bytes(),b'keep')
    def test_schema_rejects_invalid_contracts(self):
        row=self.records('convert',self.input,'--output',self.root/'output.png')[0]
        for field,value in [('schema_version',2),('ok',False),('outputs',[]),('status','unknown')]:
            altered=copy.deepcopy(row);altered[field]=value
            with self.assertRaises(ValidationError):validator.validate(altered)
        error={'schema_version':1,'type':'error','error':{'code':'invented_code','message':'x'}}
        with self.assertRaises(ValidationError):validator.validate(error)
    def test_native_classification_records(self):
        cases=['z','normal','normal_part','id','crypto','explicit_data','unknown','grey','lowercase',
               'beauty_z','beauty_normal','beauty_crypto','root_priority','ambiguous','explicit',
               'missing_part','missing_group','black','visible']
        for case in cases:
            process=subprocess.run([str(args.matrix_exe),'data_'+case],env=ENV,capture_output=True,text=True,encoding='utf-8',timeout=30)
            self.assertEqual(process.returncode,0,(case,process.stdout,process.stderr))
            checks=json.loads(process.stdout)['checks']
            validator.validate(checks['inspection']);validator.validate(checks['result'])
    def test_primary_and_sequence_records(self):
        cases=['primary_black_beauty','primary_black_root','primary_root_strategy','primary_combined','primary_tie',
               'primary_denoised','sequence_missing','sequence_no_upgrade','sequence_conflict','sequence_alpha_changed','sequence_independent']
        for case in cases:
            process=subprocess.run([str(args.matrix_exe),case],env=ENV,capture_output=True,text=True,encoding='utf-8',timeout=30)
            self.assertEqual(process.returncode,0,(case,process.stdout,process.stderr))
            for record in json.loads(process.stdout)['checks']['records']:validator.validate(record)
    def test_sequence_manifest_and_option_validation(self):
        jobs=[{'id':str(i),'sequence_id':'camera-A','input':str(self.input),'outputs':[{'path':str(self.root/(str(i)+'.png'))}]} for i in range(2)]
        path=self.root/'sequence.json';path.write_text(json.dumps({'schema_version':1,'jobs':jobs}))
        rows=self.records('batch','--manifest',path)
        self.assertEqual([r['selection']['lock_status'] for r in rows[:-1]],['created','reused'])
        self.assertTrue(all(r['sequence_id']=='camera-A' for r in rows[:-1]))
        for flags in [('--selection-policy','invalid'),('--part','0','--part-name','Beauty'),('--part-name','')]:
            self.records('convert',self.input,'--output',self.root/'invalid.png',*flags,exit=2)
        jobs[0]['sequence_id']='';path.write_text(json.dumps({'schema_version':1,'jobs':jobs}))
        self.records('batch','--manifest',path,exit=2)
    def test_stdin_boundary(self):
        self.records('batch','--manifest','-',exit=2,stdin='{"schema_version":1}')
    def test_json_lines_on_pipe(self):
        with subprocess.Popen([str(args.exe),'convert',str(self.input),'--output',str(self.root/'pipe.png'),'--json'],
                              env=ENV,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8') as process:
            first=json.loads(process.stdout.readline());validator.validate(first)
            self.assertEqual(first['type'],'result')
            rest,stderr=process.communicate(timeout=30)
            end=json.loads(rest);validator.validate(end)
            self.assertEqual(end['type'],'summary');self.assertEqual(process.returncode,0)
            self.assertEqual(stderr,'')

if __name__=='__main__':unittest.main(argv=[sys.argv[0],*remaining])
