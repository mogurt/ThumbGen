"""Public CLI contract tests. Uses only Python's standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

parser=argparse.ArgumentParser()
parser.add_argument("--exe",required=True,type=Path)
parser.add_argument("--runtime-dir",type=Path)
config,remaining=parser.parse_known_args()
EXE=config.exe.resolve()
ENV=dict(os.environ)
if config.runtime_dir:
    key="PATH" if os.name=="nt" else "LD_LIBRARY_PATH"
    ENV[key]=str(config.runtime_dir.resolve())+os.pathsep+ENV.get(key,"")


def write_png(path,width=4,height=2,rgba=(255,0,0,255)):
    def chunk(kind,data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data)&0xffffffff)
    path.parent.mkdir(parents=True,exist_ok=True)
    data=b"\x89PNG\r\n\x1a\n"
    data+=chunk(b"IHDR",struct.pack(">IIBBBBB",width,height,8,6,0,0,0))
    data+=chunk(b"sRGB",b"\0")
    data+=chunk(b"IDAT",zlib.compress((b"\0"+bytes(rgba)*width)*height))
    path.write_bytes(data+chunk(b"IEND",b""))


def png_pixels(path):
    data=path.read_bytes();offset=8;compressed=b""
    while offset<len(data):
        size=struct.unpack(">I",data[offset:offset+4])[0]
        kind=data[offset+4:offset+8];payload=data[offset+8:offset+8+size];offset+=12+size
        if kind==b"IHDR":w,h,depth,colour,_,_,interlace=struct.unpack(">IIBBBBB",payload)
        elif kind==b"IDAT":compressed+=payload
    assert depth==8 and colour in (2,6) and interlace==0
    bands=3 if colour==2 else 4
    raw=zlib.decompress(compressed);stride=w*bands;previous=bytearray(stride);result=[]
    cursor=0
    def paeth(a,b,c):
        p=a+b-c;distances=(abs(p-a),abs(p-b),abs(p-c))
        return (a,b,c)[distances.index(min(distances))]
    for _ in range(h):
        filter_type=raw[cursor];cursor+=1
        row=bytearray(raw[cursor:cursor+stride]);cursor+=stride
        for i in range(stride):
            left=row[i-bands] if i>=bands else 0
            up=previous[i];corner=previous[i-bands] if i>=bands else 0
            predictors=(0,left,up,(left+up)//2,paeth(left,up,corner))
            row[i]=(row[i]+predictors[filter_type])&255
        result.extend(tuple(row[i:i+bands]) for i in range(0,stride,bands));previous=row
    return result


class CLI(unittest.TestCase):
    def test_execution_flags(self):
        target=self.base/'budget.png'
        rows=self.run_cli('convert',self.source,'--output',target,'--threads',8,'--thread-budget',1,
                          '--memory-budget-mib',512,'--startup-timeout-ms',10000,'--task-timeout-ms',10000)
        self.assertEqual(rows[0]['execution']['requested_threads'],8)
        self.assertEqual(rows[0]['execution']['threads'],1)
        self.assertEqual(rows[0]['execution']['thread_budget'],1)
        self.assertGreater(rows[0]['execution']['estimated_task_mib'],0)
        self.assertEqual(rows[-1]['execution'],{'workers':1,'requested_workers':1,'thread_budget':1,'memory_budget_mib':512,'fixed_reserve_mib':264,'startup_timeout_ms':10000,'task_timeout_ms':10000})
        rows=self.run_cli('convert',self.source,'--output',self.base/'refused.png','--memory-budget-mib',129,exit=4)
        self.assertEqual(rows[0]['error']['code'],'task_memory_exceeds_budget')
        self.assertFalse((self.base/'refused.png').exists())
    def test_execution_validation(self):
        for flag,value in [('--workers',0),('--workers',257),('--workers',2),('--thread-budget',0),('--thread-budget',257),('--memory-budget-mib',-1),
                           ('--startup-timeout-ms',0),('--task-timeout-ms',-1),('--task-timeout-ms',86400001)]:
            with self.subTest(flag=flag,value=value):
                self.run_cli('convert',self.source,'--output',self.base/'invalid.png',flag,value,exit=2)
        self.run_cli('inspect',self.source,'--thread-budget',2,exit=2)
        self.assertFalse((self.base/'invalid.png').exists())
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="thumbgen-api-")
        self.base=Path(self.temp.name)
        self.source=self.base/"中文目录/原图.png"
        write_png(self.source)

    def tearDown(self):self.temp.cleanup()

    def run_cli(self,*args,exit=0,cwd=None):
        result=subprocess.run([str(EXE),*map(str,args),"--json"],cwd=cwd,env=ENV,
                              capture_output=True,text=True,encoding="utf-8",timeout=30)
        self.assertEqual(result.returncode,exit,(result.stdout,result.stderr))
        records=[json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(records,(result.stdout,result.stderr,args,result.returncode))
        self.assertTrue(all(r["schema_version"]==1 for r in records))
        return records

    def batch(self,jobs,defaults=None):
        path=self.base/"清单/jobs.json";path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps({"schema_version":1,"defaults":defaults or {},"jobs":jobs}),encoding="utf-8")
        return path

    def test_capabilities_defaults(self):
        r=self.run_cli("capabilities")[0]
        self.assertEqual(r["defaults"],{"format":"jpeg","quality":90,"size":[1024,1024],"preset":"balanced","background":"black"})

    def test_builtin_config_uri_in_manifest(self):
        target=self.base/"uri.png"
        manifest=self.batch([{"id":"uri","input":str(self.source),"outputs":[{"path":str(target)}]}],
                            {"ocio_config":"ocio://default"})
        record=self.run_cli("batch","--manifest",manifest)[0]
        self.assertEqual(record["colour"]["config_basis"],"explicit")
        self.assertEqual(record["colour"]["ocio_config"],"ocio://default")
        self.assertTrue(target.exists())

    def test_default_unicode_and_no_upscale(self):
        before=hashlib.sha256(self.source.read_bytes()).digest()
        r=self.run_cli("convert",self.source,"--output-dir",self.base/"输出")
        out=r[0]["outputs"][0]
        self.assertEqual(r[0]["filter"],"triangle")
        self.assertEqual((out["width"],out["height"]),(4,2))
        self.assertTrue(out["path"].endswith("原图.png.1024x1024.jpg"))
        self.assertEqual(self.run_cli("inspect",out["path"])[0]["parts"][0]["width"],4)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).digest(),before)

    def test_repeated_sizes_and_shape(self):
        write_png(self.source,120,60)
        r=self.run_cli("convert",self.source,"--output-dir",self.base/"out","--size","30x30","--size","16")
        self.assertEqual([(o["width"],o["height"]) for o in r[0]["outputs"]],[(30,15),(16,8)])

    def test_linear_light_black_background(self):
        write_png(self.source,1,1,(255,0,0,128))
        out=self.base/"black.png"
        self.run_cli("convert",self.source,"--output",out)
        pixel=png_pixels(out)[0]
        self.assertAlmostEqual(pixel[0],188,delta=2)
        self.assertEqual(pixel[1:],(0,0))

    def test_white_background(self):
        write_png(self.source,1,1,(255,0,0,128))
        out=self.base/"white.png"
        self.run_cli("convert",self.source,"--output",out,"--background","white")
        pixel=png_pixels(out)[0]
        self.assertEqual(pixel[0],255)
        self.assertAlmostEqual(pixel[1],187,delta=2)

    def test_preserve_transparent_png(self):
        write_png(self.source,1,1,(255,0,0,128))
        out=self.base/"transparent.png"
        self.run_cli("convert",self.source,"--output",out,"--background","transparent")
        pixel=png_pixels(out)[0]
        self.assertEqual(len(pixel),4)
        self.assertAlmostEqual(pixel[0],255,delta=1)
        self.assertAlmostEqual(pixel[3],128,delta=1)

    def test_jpeg_rejects_transparent(self):
        r=self.run_cli("convert",self.source,"--output",self.base/"a.jpg","--background","transparent",exit=2)
        self.assertEqual(r[0]["type"],"error")

    def test_manifest_relative_paths_bad_file_and_arbitrary_id(self):
        corrupt=self.base/"bad.png";corrupt.write_bytes(b"not a png")
        path=self.batch([
            {"id":"first image / 任意ID","input":"../中文目录/原图.png","outputs":[{"path":"../a.jpg"}]},
            {"id":"bad","input":"../bad.png","outputs":[{"path":"../b.jpg"}]},
            {"id":"last","input":"../中文目录/原图.png","outputs":[{"path":"../c.jpg"}]}])
        rows=self.run_cli("batch","--manifest",path,exit=3,cwd=Path(self.temp.name).parent)
        self.assertEqual([r["status"] for r in rows[:-1]],["success","failed","success"])
        self.assertEqual(rows[-1]["success"],2)
        self.assertTrue((self.base/"c.jpg").exists())
        self.assertFalse((self.base/"b.jpg").exists())

    def test_partial_outputs_and_no_overwrite(self):
        (self.base/"exists.jpg").write_bytes(b"keep")
        path=self.batch([{"id":"x","input":str(self.source),"outputs":[
            {"path":"../exists.jpg"},{"path":"../new.jpg"}]}])
        rows=self.run_cli("batch","--manifest",path,exit=3)
        self.assertEqual(rows[0]["status"],"partial")
        self.assertEqual(rows[0]["outputs"][0]["error"]["code"],"output_exists")
        self.assertEqual((self.base/"exists.jpg").read_bytes(),b"keep")
        self.assertTrue((self.base/"new.jpg").exists())
        self.assertFalse(list(self.base.rglob("*.tmp-*")))

    def test_duplicate_output_preflight(self):
        path=self.batch([{"id":"x","input":str(self.source),"outputs":[
            {"path":"../same.jpg"},{"path":"../same.jpg"}]}])
        self.run_cli("batch","--manifest",path,exit=2)
        self.assertFalse((self.base/"same.jpg").exists())

    def test_output_aliases_input(self):
        self.run_cli("convert",self.source,"--output",self.source,exit=2)

    def test_schema_and_unknown_field(self):
        path=self.batch([{"id":"x","input":str(self.source),"outputs":[{"path":"../x.jpg"}]}],{"preest":"fast"})
        self.run_cli("batch","--manifest",path,exit=2)
        content=json.loads(path.read_text());content["schema_version"]=2
        path.write_text(json.dumps(content))
        self.run_cli("batch","--manifest",path,exit=2)

    def test_all_failed_and_decode_limit(self):
        self.run_cli("convert",self.base/"missing.exr","--output",self.base/"a.jpg",exit=4)
        write_png(self.source,512,512)
        rows=self.run_cli("convert",self.source,"--output",self.base/"b.jpg","--max-decode-mib","1",exit=4)
        self.assertEqual(rows[0]["error"]["code"],"decode_limit_exceeded")

    def test_strict_numeric_manifest_types(self):
        job={"id":"x","input":str(self.source),"outputs":[{"path":"../x.jpg"}]}
        for values in ({"quality":True},{"quality":90.5},{"threads":4294967300},{"size":4294967297}):
            self.run_cli("batch","--manifest",self.batch([job],values),exit=2)
        self.assertFalse((self.base/"x.jpg").exists())

    @unittest.skipUnless(os.name=="nt","Windows path comparison")
    def test_case_insensitive_unicode_output_collision(self):
        path=self.batch([{"id":"x","input":str(self.source),"outputs":[
            {"path":"../ПРЕВЬЮ.png"},{"path":"../превью.png"}]}])
        self.run_cli("batch","--manifest",path,exit=2)

    def test_reject_unsupported_options(self):
        for flags in [("--preset","wrong"),("--threads","0"),("--quality","101"),("--size","0"),
                      ("--size","1bad"),("--wat","x"),("--display","monitor")]:
            self.run_cli("convert",self.source,"--output",self.base/"a.jpg",*flags,exit=2)


if __name__=="__main__":unittest.main(argv=[sys.argv[0],*remaining])
