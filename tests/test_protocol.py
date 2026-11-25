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
parser.add_argument('--control-exe',required=True,type=Path)
parser.add_argument('--isolated-exe',required=True,type=Path)
parser.add_argument('--deep-runtime-exe',required=True,type=Path)
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
    def test_cancellation_contract(self):
        error={'code':'task_cancelled','message':'interruption requested','retryable':False}
        cancelled={'path':'preview.png','existing_policy':'error','status':'cancelled','error':error}
        row={'schema_version':1,'type':'result','id':'cancelled','input':'frame.exr','status':'cancelled','ok':False,
             'outputs':[cancelled],'error':error,'warnings':[],'timings_ms':{'total':0}}
        validator.validate(row)
        committed={'path':'first.png','existing_policy':'error','status':'success','width':2,'height':1,
                   'format':'png','resize_ms':0,'finish_ms':0}
        row['outputs'].insert(0,committed);validator.validate(row)
        for change in [{'ok':True},{'error':{'code':'worker_crashed','message':'incorrect cancellation reason'}},
                       {'outputs':[]},{'outputs':[committed]},{'status':'partial'}]:
            bad=copy.deepcopy(row);bad.update(change)
            with self.assertRaises(ValidationError):validator.validate(bad)
        end={'schema_version':1,'type':'summary','total':1,'success':0,'partial':0,'failed':0,'skipped':0,
             'cancelled':1,'interrupted':True,'elapsed_ms':1,
             'output_counts':{'total':2,'success':1,'failed':0,'skipped':0,'cancelled':1,'not_attempted':0}}
        validator.validate(end)
        bad=copy.deepcopy(end);bad['interrupted']=False
        with self.assertRaises(ValidationError):validator.validate(bad)
        bad=copy.deepcopy(end);del bad['cancelled']
        with self.assertRaises(ValidationError):validator.validate(bad)
    def test_parallel_manifest(self):
        manifest={'schema_version':1,'execution':{'workers':8,'thread_budget':4,'memory_budget_mib':2048},
                  'defaults':{'threads':8},'jobs':[{'id':str(i),'input':str(self.input),'sequence_id':'pool-sequence',
                    'outputs':[{'path':str(self.root/(str(i)+'.png'))}]} for i in range(10)]}
        Draft202012Validator(json.loads((args.schema_dir/'jobs-v1.schema.json').read_text(encoding='utf-8'))).validate(manifest)
        path=self.root/'parallel.json';path.write_text(json.dumps(manifest))
        rows=self.records('batch','--manifest',path)
        self.assertEqual(rows[-1]['execution']['workers'],4)
        self.assertEqual(rows[-1]['execution']['requested_workers'],8)
        self.assertEqual({row['id'] for row in rows[:-1]},{str(i) for i in range(10)})
        for row in rows[:-1]:
            self.assertEqual(row['status'],'success')
            self.assertEqual(row['execution']['threads'],1)
            self.assertGreaterEqual(row['execution']['queue_ms'],0)
        for values in [{'workers':0},{'workers':257},{'workers':2,'thread_budget':2},
                       {'workers':2,'memory_budget_mib':1024},{'workers':2,'thread_budget':2,'memory_budget_mib':0}]:
            manifest['execution']=values;path.write_text(json.dumps(manifest))
            self.records('batch','--manifest',path,exit=2)
    def test_execution_manifest(self):
        path=self.root/'execution.json'
        manifest={'schema_version':1,'execution':{'thread_budget':2,'memory_budget_mib':512,'startup_timeout_ms':10000,'task_timeout_ms':10000},
                  'defaults':{'threads':8},'jobs':[{'id':'first','input':str(self.input),'outputs':[{'path':str(self.root/'first.png')}]},
                                                {'id':'second','input':str(self.input),'options':{'threads':4},'outputs':[{'path':str(self.root/'second.png')}]}]}
        Draft202012Validator(json.loads((args.schema_dir/'jobs-v1.schema.json').read_text())).validate(manifest)
        path.write_text(json.dumps(manifest))
        rows=self.records('batch','--manifest',path)
        self.assertEqual([row['execution']['requested_threads'] for row in rows[:-1]],[8,4])
        self.assertEqual([row['execution']['threads'] for row in rows[:-1]],[2,2])
        self.assertEqual(rows[0]['execution']['worker_id'],rows[1]['execution']['worker_id'])
        self.assertEqual(rows[-1]['execution']['thread_budget'],2)
        self.assertGreater(rows[0]['execution']['estimated_task_mib'],0)
        self.assertEqual(rows[0]['execution']['estimate_model'],'buffers-v3')
        self.assertAlmostEqual(rows[0]['execution']['estimated_task_mib']-rows[0]['execution']['admission_mib'],144)
        missing=copy.deepcopy(rows[0]);del missing['execution']['admission_mib']
        with self.assertRaises(ValidationError):validator.validate(missing)
        bad=copy.deepcopy(rows[0]);del bad['execution']['estimate_model']
        with self.assertRaises(ValidationError):validator.validate(bad)
        self.records('batch','--manifest',path,'--thread-budget',2,exit=2)
        for value in [None,[],{'workers':2},{'thread_budget':0},{'thread_budget':1.5},{'task_timeout_ms':-1},{'startup_timeout_ms':0},{'memory_budget_mib':2**64-1}]:
            with self.subTest(execution=value):
                invalid=copy.deepcopy(manifest);invalid['execution']=value
                invalid['jobs'][0]['outputs'][0]['path']=str(self.root/'never.png')
                path.write_text(json.dumps(invalid));self.records('batch','--manifest',path,exit=2)
                self.assertFalse((self.root/'never.png').exists())
    def native_records(self,executable,count,timeout=30):
        result=subprocess.run([str(executable)],env=ENV,capture_output=True,text=True,encoding='utf-8',timeout=timeout)
        self.assertEqual(result.returncode,0,result.stderr)
        records=json.loads(result.stdout)['records']
        self.assertEqual(len(records),count)
        for record in records:validator.validate(record)
        return records
    def test_control_records(self):self.native_records(args.control_exe,14)
    def test_isolated_records(self):
        enabled='integer-linear-stream-v1' in self.records('capabilities')[0]['colour_processing']
        # Reuse worker_isolated's existing native test budget (CMakeLists.txt).
        records=self.native_records(args.isolated_exe,88 if enabled else 77,timeout=60)
        c2={r['id'] for r in records if r['id'].startswith('c2-')}
        expected={'c2-normal','c2-crash','c2-after-crash','c2-partial','c2-protocol','c2-after-protocol',
                  'c2-source-change','c2-sleep','c2-low-memory','c2-cancel','c2-after-cancel'}
        self.assertEqual(c2,expected if enabled else set())
    def test_deep_runtime_records(self):self.native_records(args.deep_runtime_exe,13)
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
            for state,key in [('success','success'),('partial','partial'),('failed','failed'),('skipped','skipped'),('cancelled','cancelled')]:
                self.assertEqual(end[key],sum(r['status']==state for r in results))
            outputs=[out for r in results for out in r['outputs']]
            self.assertEqual(end['output_counts']['success'],sum(o['status']=='success' for o in outputs))
            self.assertEqual(end['output_counts']['failed'],sum(o['status']=='failed' for o in outputs))
            self.assertEqual(end['output_counts']['skipped'],sum(o['status']=='skipped' for o in outputs))
            self.assertEqual(end['output_counts']['cancelled'],sum(o['status']=='cancelled' for o in outputs))
            self.assertEqual(end['output_counts']['total'],sum(end['output_counts'][k] for k in ['success','failed','skipped','cancelled','not_attempted']))
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
        self.assertEqual(caps['quantization'],'unorm8-half-up-v1')
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
        self.assertEqual(rows[-1]['output_counts'],{'total':4,'success':2,'failed':1,'skipped':0,'cancelled':0,'not_attempted':1})
        self.assertEqual(existing.read_bytes(),b'keep')
    def test_schema_rejects_invalid_contracts(self):
        row=self.records('convert',self.input,'--output',self.root/'output.png')[0]
        for field,value in [('schema_version',2),('ok',False),('outputs',[]),('status','unknown')]:
            altered=copy.deepcopy(row);altered[field]=value
            with self.assertRaises(ValidationError):validator.validate(altered)
        error={'schema_version':1,'type':'error','error':{'code':'invented_code','message':'x'}}
        with self.assertRaises(ValidationError):validator.validate(error)
    def test_schema_selection_lock_kind_matches_mapping(self):
        lock_validator=Draft202012Validator({'$defs':schema['$defs'],'$ref':'#/$defs/selection_key'})
        opacity=dict(part=0,part_name='',kind='deep',group='A',channels=['A','Z'],
                     deep=dict(policy='deep-opacity-mapping-v1',alpha='A',z='Z',zback=''))
        colour=dict(part_name='image',kind='colour',group='RGB',channels=['R','G','B'],
                     deep=dict(policy='deep-mapping-v2',colour=['R','G','B'],alpha=['A']*3,
                               z='Z',zback='ZBack',shared_alpha=True))
        flat={key:value for key,value in colour.items() if key!='deep'}
        grey=dict(colour,kind='grey',group='GRAY',channels=['Y']*3,deep=dict(colour['deep'],colour=['Y']*3))
        data=dict(flat,kind='data',group='Z',channels=['Z'])
        for record in (opacity,colour,grey,flat,data):
            lock_validator.validate(record)
        for record in (
            {key:value for key,value in opacity.items() if key!='deep'},
            dict(opacity,deep=colour['deep']),
            *(dict(record,deep=opacity['deep']) for record in (colour,grey,data)),
            dict(data,deep=colour['deep']),
        ):
            with self.subTest(record=record),self.assertRaises(ValidationError):lock_validator.validate(record)
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
    def test_existing_policies_and_counts(self):
        target=self.root/'existing.png';target.write_bytes(b'keep')
        before=target.stat().st_mtime_ns
        rows=self.records('convert',self.input,'--output',target,'--existing','skip')
        self.assertEqual(rows[0]['status'],'skipped');self.assertNotIn('decode',rows[0]['timings_ms'])
        self.assertEqual(rows[-1]['skipped'],1);self.assertEqual(rows[-1]['output_counts']['skipped'],1)
        self.assertEqual(target.read_bytes(),b'keep');self.assertEqual(target.stat().st_mtime_ns,before)
        rows=self.records('convert',self.input,'--output',target,'--existing','replace')
        self.assertEqual(rows[0]['status'],'success');self.assertNotEqual(target.read_bytes(),b'keep')
        target.write_bytes(b'keep')
        jobs=[{'id':'mixed','input':str(self.input),'outputs':[
            {'path':str(target),'existing':'skip'},{'path':str(self.root/'new.png'),'existing':'replace'}]}]
        path=self.root/'policies.json';path.write_text(json.dumps({'schema_version':1,'defaults':{'existing':'error'},'jobs':jobs}))
        rows=self.records('batch','--manifest',path)
        self.assertEqual(rows[0]['status'],'success');self.assertEqual(rows[-1]['output_counts']['skipped'],1)
        self.assertEqual(rows[-1]['output_counts']['success'],1)
        self.records('convert',self.input,'--output',target,'--existing','invalid',exit=2)
        self.assertFalse(list(self.root.rglob('.thumbgen-tmp-*')))
    def test_hardlink_alias_preflight(self):
        linked=self.root/'linked.png';os.link(self.input,linked)
        original=self.input.read_bytes()
        self.records('convert',self.input,'--output',linked,'--existing','replace',exit=2)
        self.assertEqual(self.input.read_bytes(),original)
        other=self.root/'other.png';other.write_bytes(b'other')
        duplicate=self.root/'duplicate.png';os.link(other,duplicate)
        path=self.root/'aliases.json';path.write_text(json.dumps({'schema_version':1,'jobs':[
            {'id':'aliases','input':str(self.input),'outputs':[{'path':str(other)},{'path':str(duplicate)}]}]}))
        self.records('batch','--manifest',path,exit=2)
        self.assertEqual(other.read_bytes(),b'other')
    def test_native_file_records(self):
        cases=['io_error','io_skip','io_replace','io_partial','io_hardlink','io_race_error','io_race_skip','io_race_replace',
               'io_source_decode','io_source_identity','io_source_commit','io_source_partial','io_cleanup_foreign']
        if os.name!='nt':
            cases+=['io_symlink','io_write_limit']
            if os.geteuid()!=0:cases+=['io_permission']
        for case in cases:
            process=subprocess.run([str(args.matrix_exe),case],env=ENV,capture_output=True,text=True,encoding='utf-8',timeout=30)
            self.assertEqual(process.returncode,0,(case,process.stdout,process.stderr))
            for record in json.loads(process.stdout)['checks']['records']:validator.validate(record)
    def test_long_paths_and_replacement(self):
        base=self.root.joinpath(*['long-path-'+str(i)+'x'*42 for i in range(6)])
        if os.name=='nt':base=Path('\\\\?\\'+str(base))
        base.mkdir(parents=True)
        source=base/'source.png';source.write_bytes(self.input.read_bytes())
        target=base/'output.png';target.write_bytes(b'old-long-target')
        rows=self.records('convert',source,'--output',target,'--existing','replace')
        self.assertEqual(rows[0]['status'],'success');self.assertNotEqual(target.read_bytes(),b'old-long-target')
        self.assertFalse(list(base.glob('.thumbgen-tmp-*')))
        # Remove through the extended Windows path as well, without relying on
        # the host's unprefixed long-path policy during TemporaryDirectory cleanup.
        source.unlink();target.unlink()
        for _ in range(6):base.rmdir();base=base.parent
    def test_leaf_symlink_is_not_followed(self):
        actual=self.root/'actual.png';actual.write_bytes(b'leave-actual')
        linked=self.root/'link.png'
        try:linked.symlink_to(actual)
        except OSError as error:
            if os.name=='nt' and getattr(error,'winerror',None)==1314:self.skipTest('Windows symlink privilege unavailable')
            raise
        rows=self.records('convert',self.input,'--output',linked,'--existing','replace',exit=4)
        self.assertEqual(rows[0]['outputs'][0]['error']['code'],'output_symlink')
        self.assertEqual(actual.read_bytes(),b'leave-actual');self.assertTrue(linked.is_symlink())
    @unittest.skipUnless(os.name=='nt','Windows sharing semantics')
    def test_replace_locked_target_preserves_old_file(self):
        import ctypes
        from ctypes import wintypes
        target=self.root/'locked.png';target.write_bytes(b'locked-old')
        create=ctypes.windll.kernel32.CreateFileW
        create.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
        create.restype=wintypes.HANDLE
        handle=create(str(target),0x80000000,1,None,3,0,None) # READ, share READ only: no DELETE sharing.
        self.assertNotEqual(handle,wintypes.HANDLE(-1).value)
        try:
            rows=self.records('convert',self.input,'--output',target,'--existing','replace',exit=4)
            self.assertEqual(rows[0]['outputs'][0]['error']['code'],'commit_failed')
            self.assertEqual(target.read_bytes(),b'locked-old')
            self.assertFalse(list(self.root.rglob('.thumbgen-tmp-*')))
        finally:
            close=ctypes.windll.kernel32.CloseHandle;close.argtypes=[wintypes.HANDLE];close(handle)
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
