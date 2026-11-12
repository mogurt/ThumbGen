"""Check the public preset policy, real zlib levels, adaptive filters and alpha."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib
from PIL import Image
from jsonschema import Draft202012Validator

p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--runtime-dir',type=Path,required=True);p.add_argument('--schema-dir',type=Path,required=True)
a,remaining=p.parse_known_args();env=dict(os.environ,OCIO='ocio://default')
key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH';env[key]=str(a.runtime_dir.resolve())+os.pathsep+env.get(key,'')
validator=Draft202012Validator(json.loads((a.schema_dir/'protocol-v1.schema.json').read_text()))

def chunk(k,b):return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b)&0xffffffff)

def chunks(path):
    data=path.read_bytes();offset=8;result=[]
    while offset<len(data):
        n=struct.unpack_from('>I',data,offset)[0];result.append((data[offset+4:offset+8],data[offset+8:offset+8+n]));offset+=n+12
    return result

def cli(*args):
    run=subprocess.run([str(a.exe.resolve()),*map(str,args),'--json'],env=env,capture_output=True,text=True,encoding='utf-8',timeout=30)
    assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
    rows=[json.loads(line) for line in run.stdout.splitlines()]
    for row in rows:validator.validate(row)
    return rows

class Encoding(unittest.TestCase):
    def test_capabilities(self):
        self.assertEqual(cli('capabilities')[0]['png_encoding'],dict(compression_levels=dict(fast=1,balanced=3,quality=6),filter='adaptive'))

    def test_presets_override_source_controls(self):
        with tempfile.TemporaryDirectory(prefix='png-policy-') as tmp:
            root=Path(tmp);width,height=320,192
            for rgba in (False,True):
                nc=4 if rgba else 3
                pixels=bytes(v for y in range(height) for x in range(width) for v in ([x%256,y%256,(x+y)%256]+([0 if x<32 else (x*7)%256] if rgba else [])))
                raw=b''.join(b'\0'+pixels[y*width*nc:(y+1)*width*nc] for y in range(height))
                source=root/f'source-{nc}.png'
                source.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6 if rgba else 2,0,0,0))+chunk(b'sRGB',b'\0')+chunk(b'tEXt',b'png:compressionLevel\x009')+chunk(b'tEXt',b'png:filter\x008')+chunk(b'tEXt',b'Description\x00preset-policy-fixture')+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
                for preset,hint in [('fast',0),('balanced',1),('quality',2)]:
                    target=root/f'{nc}-{preset}.png'
                    record=cli('convert',source,'--output',target,'--size','320x192','--preset',preset,'--threads','1','--background','transparent' if rgba else 'black')
                    self.assertTrue(record[0]['ok']);self.assertEqual(record[0]['preset'],preset)
                    payload=b''.join(b for k,b in chunks(target) if k==b'IDAT')
                    self.assertEqual(payload[1]>>6,hint,preset)
                    scan=zlib.decompress(payload);filters=scan[::1+width*nc]
                    self.assertEqual(len(filters),height);self.assertTrue(any(f!=0 for f in filters),'adaptive filtering was overridden')
                    with Image.open(target) as image:
                        self.assertEqual(image.size,(width,height))
                        if rgba:
                            actual=image.getchannel('A').tobytes();self.assertEqual(actual,pixels[3::4])
                            self.assertTrue(all(pixel[:3]==(0,0,0) for pixel in image.getdata() if pixel[3]==0))

if __name__=='__main__':unittest.main(argv=[__file__,*remaining])
