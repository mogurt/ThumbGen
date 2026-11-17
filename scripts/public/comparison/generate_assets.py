"""Reproducible synthetic PNG fixtures (CC0-1.0). Not photographic sensor data.
Uses integer pixels and standard-library PNG encoding; run outside timed windows.
"""
import argparse,hashlib,json,struct,zlib
from pathlib import Path

def chunk(f,tag,data):
    f.write(struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xffffffff))

def png(path,w,h,channels,pattern,bits=16):
    with path.open('xb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        chunk(f,b'IHDR',struct.pack('>IIBBBBB',w,h,bits,2 if channels==3 else 6,0,0,0))
        chunk(f,b'sRGB',b'\0')
        compressor=zlib.compressobj(6)
        for y in range(h):
            row=bytearray(b'\0')
            for x in range(w):
                if pattern=='gradient':values=[x*65535//max(1,w-1),y*65535//max(1,h-1),(x+y)*65535//max(1,w+h-2)]
                else:values=[((x*257)^(y*911)^(x*y*13))&65535,((x*1237)^(y*31)^(x*y*7))&65535,((x*71)^(y*733)^(x*y*19))&65535]
                if channels==4:values.append((x*163+y*277)&65535)
                row.extend(struct.pack('>'+'H'*channels,*values) if bits==16 else bytes(v>>8 for v in values))
            data=compressor.compress(row)
            if data:chunk(f,b'IDAT',data)
        chunk(f,b'IDAT',compressor.flush());chunk(f,b'IEND',b'')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=False);rows=[]
    configs=[(w,h,c,pattern,16) for w,h in [(4000,2000),(6000,4000),(2000,4000)] for c in [3,4] for pattern in ['gradient','detail']]
    configs.append((256,192,3,'detail',8))
    for w,h,c,pattern,bits in configs:
        name=f'synthetic-{w}x{h}-{c}ch-{pattern}-{bits}.png';path=args.output/name
        png(path,w,h,c,pattern,bits)
        rows.append({'id':path.stem,'filename':name,'width':w,'height':h,'channels':c,'bits':bits,'pattern':pattern,'synthetic':True,'license':'CC0-1.0','sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'bytes':path.stat().st_size})
        print(name,flush=True)
    (args.output/'generated-assets.json').write_text(json.dumps({'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'python_zlib':zlib.ZLIB_RUNTIME_VERSION,'files':rows},indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
