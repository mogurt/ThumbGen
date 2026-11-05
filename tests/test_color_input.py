"""ICC/CMYK policy and fixed SDK probes, with independently authored containers.

No product/OIIO writer, colour processor or generated product pixels are used
as the oracle. PNG/TIFF/JPEG fixtures below follow the respective file layouts.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zlib
from jsonschema import Draft202012Validator

parser=argparse.ArgumentParser()
for name in ('exe','probe-exe','runtime-dir','schema-dir'):
    parser.add_argument('--'+name,type=Path,required=True)
args,remaining=parser.parse_known_args()
env=dict(os.environ,OCIO='')
key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
env[key]=str(args.runtime_dir.resolve())+os.pathsep+env.get(key,'')
validator=Draft202012Validator(json.loads((args.schema_dir/'protocol-v1.schema.json').read_text(encoding='utf-8')))

def read_png(path):
    # Standard-library decoder, following test_cryptomatte_cli's independent I/O.
    data=path.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n'
    position=8;compressed=b''
    while position+12<=len(data):
        count=struct.unpack_from('>I',data,position)[0]
        tag=data[position+4:position+8];payload=data[position+8:position+8+count]
        assert len(payload)==count and zlib.crc32(tag+payload)==struct.unpack_from('>I',data,position+8+count)[0]
        if tag==b'IHDR':width,height,depth,kind,compression,filtering,interlace=struct.unpack('>IIBBBBB',payload)
        if tag==b'IDAT':compressed+=payload
        position+=count+12
        if tag==b'IEND':break
    assert depth==8 and kind in (2,6) and (compression,filtering,interlace)==(0,0,0)
    bands=3 if kind==2 else 4;stride=width*bands;raw=zlib.decompress(compressed)
    assert len(raw)==height*(stride+1)
    previous=bytearray(stride);pixels=[]
    for y in range(height):
        start=y*(stride+1);mode=raw[start];row=bytearray(raw[start+1:start+1+stride]);assert mode in range(5)
        for x in range(stride):
            a=row[x-bands] if x>=bands else 0;b=previous[x];c=previous[x-bands] if x>=bands else 0
            p=a+b-c;distances=(abs(p-a),abs(p-b),abs(p-c))
            prediction=(0,a,b,(a+b)//2,(a,b,c)[distances.index(min(distances))])[mode]
            row[x]=(row[x]+prediction)&255
        pixels.extend(tuple(row[x:x+bands]) for x in range(0,stride,bands));previous=row
    return width,height,pixels

def chunk(tag,data):
    return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xffffffff)

def profile():
    """Small ICC v2 RGB monitor matrix profile: D50 PCS, unit TRCs.

    These colourants are the D50-adapted Rec.709 matrix, encoded s15Fixed16.
    This is a calibration fixture, not a production profile/ICC oracle.
    """
    def xyz(values):
        return b'XYZ '+b'\0'*4+b''.join(struct.pack('>i',round(x*65536)) for x in values)
    tags=[(b'desc',b'desc'+b'\0'*4+struct.pack('>I',12)+b'Calibration\0'+b'\0'*78),
          (b'cprt',b'text'+b'\0'*4+b'ThumbGen synthetic calibration\0'),
          (b'wtpt',xyz((0.9642,1,0.8249))),
          (b'rXYZ',xyz((0.4360747,0.2225045,0.0139322))),
          (b'gXYZ',xyz((0.3850649,0.7168786,0.0971045))),
          (b'bXYZ',xyz((0.1430804,0.0606169,0.7141733)))]
    tags += [(name,b'curv'+b'\0'*4+struct.pack('>IH',1,256)) for name in (b'rTRC',b'gTRC',b'bTRC')]
    offset=132+12*len(tags);table=struct.pack('>I',len(tags));body=b''
    for name,data in tags:
        table+=name+struct.pack('>II',offset,len(data))
        padded=data+b'\0'*((-len(data))%4);body+=padded;offset+=len(padded)
    header=bytearray(128)
    struct.pack_into('>I',header,0,offset);header[8:12]=bytes((2,0x10,0,0))
    header[12:24]=b'mntrRGB XYZ '
    struct.pack_into('>6H',header,24,2026,1,1,0,0,0)
    header[36:40]=b'acsp';header[40:44]=b'MSFT'
    header[68:80]=xyz((0.9642,1,0.8249))[8:]
    return bytes(header)+table+body

def png(icc=None):
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,2,0,0,0))
    if icc is not None:data+=chunk(b'iCCP',b'Calibration\0\0'+zlib.compress(icc))
    return data+chunk(b'IDAT',zlib.compress((b'\0'+bytes((64,128,192))*8)*8))+chunk(b'IEND',b'')

def jpeg(cmyk=False,icc=None):
    """8x8 constant 128 samples, unit quantizer, DC=0, AC=EOB.

    The two one-bit Huffman codes encode zero DC difference and EOB; each
    component block is 00. APP14 transform=0 marks inverted Adobe CMYK.
    """
    def segment(marker,data):return bytes((255,marker))+struct.pack('>H',len(data)+2)+data
    n=4 if cmyk else 3
    data=b'\xff\xd8'
    if cmyk:data+=segment(0xee,b'Adobe'+struct.pack('>HHHB',100,0,0,0))
    if icc is not None:data+=segment(0xe2,b'ICC_PROFILE\0'+bytes((1,1))+icc)
    data+=segment(0xdb,b'\0'+b'\1'*64)
    data+=segment(0xc0,struct.pack('>BHHB',8,8,8,n)+b''.join(bytes((i+1,0x11,0)) for i in range(n)))
    data+=segment(0xc4,b'\0'+b'\1'+b'\0'*15+b'\0'+b'\x10'+b'\1'+b'\0'*15+b'\0')
    data+=segment(0xda,bytes((n,))+b''.join(bytes((i+1,0)) for i in range(n))+bytes((0,63,0)))
    bits='00'*n;bits+='1'*((-len(bits))%8)
    return data+int(bits,2).to_bytes(len(bits)//8,'big')+b'\xff\xd9'

def tiff(cmyk=False,icc=None,inkset=1,endian='<',big=False):
    n=4 if cmyk else 3
    tags=[(256,4,1,8),(257,4,1,8),(258,3,n,None),(259,3,1,1),
          (262,3,1,5 if cmyk else 2),(273,4,1,None),(277,3,1,n),
          (278,4,1,8),(279,4,1,64*n),(284,3,1,1)]
    if cmyk:tags.append((332,3,1,inkset))
    if icc is not None:tags.append((34675,7,len(icc),None))
    tags.sort();offset=(16+8+20*len(tags)+8) if big else (8+2+12*len(tags)+4)
    # BigTIFF can keep BitsPerSample inline (at most 8 bytes).
    body=b'' if big else struct.pack(endian+'H'*n,*([8]*n))
    bits_offset=offset;offset+=len(body)
    icc_offset=offset
    if icc is not None:body+=icc;offset+=len(icc)
    order=b'II' if endian=='<' else b'MM'
    result=order+(struct.pack(endian+'HHHQQ',43,8,0,16,len(tags)) if big else struct.pack(endian+'HIH',42,8,len(tags)))
    for tag,kind,count,value in tags:
        if tag==258:value=bits_offset
        if tag==273:value=offset
        if tag==34675:value=icc_offset
        result+=struct.pack(endian+('HHQ' if big else 'HHI'),tag,kind,count)
        width=8 if big else 4
        if tag==34675 and count<=width:result+=icc.ljust(width,b'\0')
        elif tag==258 and big:result+=struct.pack(endian+'H'*n,*([8]*n)).ljust(width,b'\0')
        else:result+=struct.pack(endian+('H' if kind==3 and count==1 else 'I'),value).ljust(width,b'\0')
    samples=bytes((0,64,128,32)) if cmyk else bytes((64,128,192))
    return result+b'\0'*(8 if big else 4)+body+samples*64

def psd(mode=4):
    # Flat, uncompressed, 8-bit 4-channel PSD composite. PSD stores inverted inks.
    return b'8BPS'+struct.pack('>H6sHIIHH',1,b'\0'*6,4,8,8,8,mode)+b'\0'*12+b'\0\0'+bytes((128,))*256

class ColourInput(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='thumbgen-colour-')
        self.root=Path(self.temp.name);self.target=self.root/'output.png'
    def tearDown(self):self.temp.cleanup()
    def fixture(self,name,data):
        path=self.root/name;path.write_bytes(data);return path
    def run_json(self,exe,command,exit=0):
        command=[str(exe),*map(str,command)]
        run=subprocess.run(command,env=env,capture_output=True,text=True,encoding='utf-8',timeout=45)
        diagnostic=f'command={command!r}\nreturncode={run.returncode}\nstdout={run.stdout!r}\nstderr={run.stderr!r}'
        self.assertEqual(run.returncode,exit,diagnostic)
        try:rows=[json.loads(line) for line in run.stdout.splitlines() if line.strip()]
        except json.JSONDecodeError as error:self.fail(f'invalid JSON: {error}\n{diagnostic}')
        self.assertTrue(rows,f'no JSON records\n{diagnostic}')
        return rows
    def convert(self,path,space=None,exit=0):
        before=hashlib.sha256(path.read_bytes()).digest()
        command=['convert',path,'--output',self.target,'--format','png','--size','8x8',
                 '--preset','fast','--ocio-config','ocio://default','--threads',1,'--json']
        if space is not None:command+=['--input-space',space]
        rows=self.run_json(args.exe,command,exit)
        for row in rows:validator.validate(row)
        self.assertEqual(before,hashlib.sha256(path.read_bytes()).digest())
        return rows[0]
    def reject(self,path,space=None):
        result=self.convert(path,space,4)
        self.assertEqual(result['error']['code'],'colour_input_unsupported',result)
        self.assertFalse(self.target.exists())
    def test_icc_auto_rejected(self):
        for extension,make in [('png',lambda:png(profile())),('jpg',lambda:jpeg(icc=profile())),('tif',lambda:tiff(icc=profile()))]:
            with self.subTest(extension=extension):
                path=self.fixture('calibrated.'+extension,make())
                probe=self.run_json(args.probe_exe,[path])[0]
                self.assertTrue(probe['icc'],probe)
                expected=(128,128,128) if extension=='jpg' else (64,128,192)
                for actual,value in zip(probe['first'],expected):self.assertAlmostEqual(actual,value/255,places=6)
                self.reject(path)
    def test_bad_icc_rejected_even_if_decoder_discards(self):
        for extension,data in [('png',png(b'bad')),('jpg',jpeg(icc=b'bad')),('tif',tiff(icc=b'bad')),('empty.tif',tiff(icc=b''))]:
            with self.subTest(extension=extension):self.reject(self.fixture('bad.'+extension,data))
    def test_incomplete_jpeg_icc_sequence(self):
        data=jpeg(icc=profile()).replace(b'ICC_PROFILE\0\1\1',b'ICC_PROFILE\0\1\2')
        path=self.fixture('incomplete.jpg',data)
        self.assertFalse(self.run_json(args.probe_exe,[path])[0]['icc'])
        self.reject(path)
    def test_tiff_icc_tag_all_directory_layouts(self):
        for endian in ('<','>'):
            for big in (False,True):
                for icc in (b'',profile()):
                    with self.subTest(endian=endian,big=big,size=len(icc)):
                        self.reject(self.fixture('directory.tif',tiff(icc=icc,endian=endian,big=big)))
    def test_explicit_linear_override_independent_reference(self):
        for extension,data in [('png',png(profile())),('tif',tiff(icc=profile())),('bad.png',png(b'bad'))]:
            with self.subTest(extension=extension):
                self.target=self.root/('out-'+extension+'.png')
                result=self.convert(self.fixture('override.'+extension,data),'linear')
                self.assertTrue(result['ok'],result)
                self.assertTrue(any('bypassed' in warning for warning in result['warnings']))
                # IEC sRGB piecewise encoding of authored linear Rec.709 bytes.
                def srgb(x):return 12.92*x if x<=0.0031308 else 1.055*x**(1/2.4)-0.055
                expected=tuple(math.floor(255*srgb(x/255)+0.5) for x in (64,128,192))
                w,h,pixels=read_png(self.target);self.assertEqual((w,h),(8,8))
                for pixel in pixels:
                    for actual,want in zip(pixel[:3],expected):self.assertLessEqual(abs(actual-want),1)
    def test_unprofiled_rgb_unchanged(self):
        result=self.convert(self.fixture('plain.png',png()))
        self.assertTrue(result['ok'],result)
        self.assertFalse(any('ICC' in warning for warning in result['warnings']))
        self.assertEqual(read_png(self.target)[2],[(64,128,192)]*64)
    def test_cmyk_rejects_auto_and_explicit(self):
        ycck=jpeg(True).replace(b'Adobe\0d\0\0\0\0\0',b'Adobe\0d\0\0\0\0\2')
        for extension,data in [('jpg',jpeg(True)),('ycck.jpg',ycck),('tif',tiff(True)),('icc.tif',tiff(True,profile())),('separated.tif',tiff(True,inkset=2)),('psd',psd()),('separated.psd',psd(7))]:
            for space in (None,'sRGB','linear'):
                with self.subTest(extension=extension,space=space):self.reject(self.fixture('cmyk.'+extension,data),space)
    def test_decoder_cmyk_formula_is_not_icc_transform(self):
        path=self.fixture('cmyk.tif',tiff(True))
        normal=self.run_json(args.probe_exe,[path])[0];raw=self.run_json(args.probe_exe,[path,'raw'])[0]
        self.assertEqual(normal['tiff'],'CMYK');self.assertEqual(normal['channels'],['R','G','B'])
        self.assertEqual(raw['channels'],['C','M','Y','K'])
        for actual,value in zip(raw['first'],(0,64,128,32)):self.assertAlmostEqual(actual,value/255,places=6)
        # For these samples, TIFF's (1-C)*(1-K) yields these 8-bit codes.
        for actual,value in zip(normal['first'],(223,167,111)):self.assertAlmostEqual(actual,value/255,places=6)
        path=self.fixture('cmyk.jpg',jpeg(True))
        normal=self.run_json(args.probe_exe,[path])[0];raw=self.run_json(args.probe_exe,[path,'raw'])[0]
        self.assertEqual(normal['jpeg'],'CMYK');self.assertEqual(normal['channels'],['R','G','B'])
        self.assertEqual(raw['channels'],['R','G','B']) # RawColor is ignored for JPEG.
        for value in normal['first']:self.assertAlmostEqual(value,64/255,places=6)
    def test_batch_failure_then_explicit_success(self):
        path=self.fixture('icc.png',png(profile()))
        jobs=[{'id':name,'input':str(path),'options':{'input_space':space,'ocio_config':'ocio://default'},
               'outputs':[{'path':str(self.root/(name+'.png')),'format':'png','size':[8,8]}]}
              for name,space in [('auto','auto'),('override','linear')]]
        manifest=self.root/'jobs.json';manifest.write_text(json.dumps({'schema_version':1,'jobs':jobs}),encoding='utf-8')
        rows=self.run_json(args.exe,['batch','--manifest',manifest,'--json'],3)
        for row in rows:validator.validate(row)
        results={row['id']:row for row in rows if row['type']=='result'}
        self.assertEqual(results['auto']['error']['code'],'colour_input_unsupported')
        self.assertTrue(results['override']['ok'])
        self.assertFalse((self.root/'auto.png').exists());self.assertTrue((self.root/'override.png').exists())
    def test_presence_scanner_headers(self):
        # Scanner unit tests: synthetic RIFF headers need not decode as images.
        def riff(chunks):return b'RIFF'+struct.pack('<I',4+len(chunks))+b'WEBP'+chunks
        samples=[(b'',False),(b'not an image',False),(png(),False),(jpeg(),False),
                 (tiff(),False),(riff(b'VP8 '+struct.pack('<I',1)+b'x\0'),False),
                 (riff(b'VP8 '+struct.pack('<I',1)+b'x\0ICCP'+b'\0'*4),True),
                 (b'\x89PNG\r\n\x1a\n'+struct.pack('>I',0xffffffff)+b'junk',False),
                 (b'\xff\xd8\xff\xe2\0\1junk',False)]
        for data,expected in samples:
            with self.subTest(data=data[:12]):
                path=self.fixture('container.bin',data)
                self.assertEqual(self.run_json(args.probe_exe,['--presence',path])[0]['icc'],expected)
    def test_presence_probe_repeated(self):
        # Every launch must succeed: these are independent assertions, not
        # retries that discard a missing response. Exercise exit-time flushing.
        path=self.fixture('empty.bin',b'')
        for index in range(50):
            with self.subTest(index=index):
                self.assertEqual(self.run_json(args.probe_exe,['--presence',path]),[{'icc':False}])
    def test_subprocess_diagnostics(self):
        for code,stdout,reason in [(0,'','no JSON records'),(0,'not-json','invalid JSON'),(7,'','7 != 0')]:
            with self.subTest(code=code,stdout=stdout):
                response=subprocess.CompletedProcess([],code,stdout,'diagnostic stderr')
                with mock.patch.object(subprocess,'run',return_value=response):
                    with self.assertRaises(AssertionError) as caught:self.run_json('fake-probe',['--presence','empty.bin'])
                message=str(caught.exception)
                for text in (reason,'fake-probe','--presence','empty.bin',f'returncode={code}',f'stdout={stdout!r}',"stderr='diagnostic stderr'"):
                    self.assertIn(text,message)

if __name__=='__main__':unittest.main(argv=[__file__,*remaining])
