"""Exercise real CLI console/POSIX signals without touching the test runner's console."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from jsonschema import Draft202012Validator


def helper(exe,manifest,kind):
    handler=None
    if os.name=='nt':
        callback=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_ulong)
        handler=callback(lambda event:1 if event in (0,1) else 0)
        if not ctypes.windll.kernel32.SetConsoleCtrlHandler(handler,True):raise ctypes.WinError()
    peer_options=dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name=='nt' else dict(start_new_session=True)
    peer=subprocess.Popen([str(exe),'--internal-worker','0'*32,'independent', '1'],stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE,**peer_options)
    process=subprocess.Popen([str(exe),'batch','--manifest',str(manifest),'--json'],stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        first=process.stdout.readline()
        if not first:raise RuntimeError('CLI exited before its first result')
        # The helper owns a private, hidden console on Windows. Broadcast stays there.
        for _ in range(2):
            if os.name=='nt':
                event=0 if kind=='int' else 1
                if not ctypes.windll.kernel32.GenerateConsoleCtrlEvent(event,0):raise ctypes.WinError()
            elif process.poll() is None:os.kill(process.pid,signal.SIGINT if kind=='int' else signal.SIGTERM)
        rest,stderr=process.communicate(timeout=30)
        if peer.poll() is not None:raise RuntimeError('cancellation terminated an independent thumbgen instance')
        print(json.dumps({'exit':process.returncode,'records':[json.loads(line) for line in (first+rest).splitlines()],
                          'stderr':stderr}))
    finally:
        if process.poll() is None:process.kill();process.wait()
        peer.kill();peer.communicate(timeout=5)


def chunk(kind,payload):
    return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)


class Interrupt(unittest.TestCase):
    def test_real_signals(self):
        schema=json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8'))
        validator=Draft202012Validator(schema)
        for kind in ('int','break' if os.name=='nt' else 'term'):
            with self.subTest(signal=kind),tempfile.TemporaryDirectory(prefix='thumbgen-interrupt-') as directory:
                root=Path(directory);source=root/'source.png'
                original=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\xff\0\0\xff\0\0'))+chunk(b'IEND',b'')
                source.write_bytes(original)
                jobs=[{'id':str(i),'input':str(source),'outputs':[{'path':str(root/(str(i)+'.png'))}]} for i in range(256)]
                manifest=root/'jobs.json';manifest.write_text(json.dumps({'schema_version':1,'execution':{'workers':2,'thread_budget':2,'memory_budget_mib':1024},'jobs':jobs}))
                options={}
                if os.name=='nt':
                    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
                    options=dict(creationflags=subprocess.CREATE_NEW_CONSOLE,startupinfo=startup)
                result=subprocess.run([sys.executable,__file__,'--helper',str(args.exe),str(manifest),kind],
                                      env=ENV,capture_output=True,text=True,encoding='utf-8',timeout=40,**options)
                self.assertEqual(result.returncode,0,result.stderr)
                data=json.loads(result.stdout);self.assertEqual(data['exit'],130,data)
                rows=data['records'];self.assertEqual(len(rows),len(jobs)+1)
                for row in rows:validator.validate(row)
                end=rows[-1];self.assertEqual(end['type'],'summary');self.assertTrue(end['interrupted'])
                self.assertGreater(end['cancelled'],0);self.assertGreater(end['success'],0)
                self.assertEqual({row['id'] for row in rows[:-1]},{job['id'] for job in jobs})
                self.assertEqual(end['total'],sum(end[key] for key in ('success','partial','failed','skipped','cancelled')))
                outputs=[out for row in rows[:-1] for out in row['outputs']]
                for state in ('success','failed','skipped','cancelled'):
                    self.assertEqual(end['output_counts'][state],sum(out['status']==state for out in outputs))
                for out in outputs:
                    path=Path(out['path'])
                    if out['status']=='success':self.assertTrue(path.read_bytes().startswith(b'\x89PNG'))
                    elif out['status']=='cancelled':self.assertFalse(path.exists())
                self.assertEqual(source.read_bytes(),original)
                self.assertFalse(list(root.rglob('.thumbgen-tmp-*')))


if __name__=='__main__':
    if len(sys.argv)==5 and sys.argv[1]=='--helper':helper(Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4])
    else:
        parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True)
        parser.add_argument('--schema-dir',type=Path,required=True);parser.add_argument('--runtime-dir',type=Path)
        args,remaining=parser.parse_known_args();ENV=dict(os.environ,OCIO='ocio://default')
        if args.runtime_dir:
            key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
            ENV[key]=str(args.runtime_dir.resolve())+os.pathsep+ENV.get(key,'')
        unittest.main(argv=[sys.argv[0],*remaining])
