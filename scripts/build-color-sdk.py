"""Build pinned colour dependencies into a NEW private prefix.

No existing SDK is edited. CMS is library-only with no GPL plugins. A shared /MD
JPEG avoids mixing the existing source SDK's /MT static JPEG into the CLI.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--root',type=Path,required=True)
parser.add_argument('--lcms-cache',type=Path)
parser.add_argument('--jpeg-cache',type=Path)
args=parser.parse_args()
root=args.root.resolve()
if root.exists():parser.error('--root must not exist; old SDKs and evidence are never overwritten')
root.mkdir(parents=True)
lock=json.loads((Path(__file__).resolve().parents[1]/'dependencies/color-sdk.json').read_text())
records=[]
def run(command,name):
    result=subprocess.run(list(map(str,command)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (root/(name+'.log')).write_bytes(result.stdout)
    if result.returncode:raise RuntimeError(f'{name} failed ({result.returncode}); see {root/(name+".log")}')
    return result.stdout.decode('utf-8',errors='replace').strip()
for package,cache in [('LittleCMS2',args.lcms_cache),('JPEG',args.jpeg_cache)]:
    item=lock[package];source=root/(package+'-source');build=root/(package+'-build')
    run(['git','clone','--no-checkout',str(cache.resolve()) if cache else item['repository'],source],package+'-clone')
    run(['git','-C',source,'checkout','--detach',item['commit']],package+'-checkout')
    actual=run(['git','-C',source,'rev-parse','HEAD'],package+'-identity')
    if actual!=item['commit']:raise RuntimeError('source commit mismatch')
    command=['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+str(root/'sdk'),
             '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL']
    if os.name=='nt':command+=['-G','Visual Studio 17 2022','-A','x64']
    else:command+=['-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic']
    command+=['-D'+option for option in item['options']]
    run(command,package+'-configure')
    run(['cmake','--build',build,'--config','Release','--parallel','2'],package+'-build')
    run(['cmake','--install',build,'--config','Release'],package+'-install')
    license=source/item['license_file']
    target=root/'sdk'/'share'/'thumbgen-color-licenses'/package/item['license_file']
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(license.read_bytes())
    if package=='JPEG':(target.parent/'README.ijg').write_bytes((source/'README.ijg').read_bytes())
    records.append(dict(package=package,**item,license_sha256=hashlib.sha256(license.read_bytes()).hexdigest()))
files={str(p.relative_to(root/'sdk')):hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'sdk').rglob('*') if p.is_file()}
(root/'receipt.json').write_text(json.dumps(dict(components=records,installed_sha256=files),indent=2)+'\n')
print(json.dumps(dict(prefix=str(root/'sdk'),receipt=str(root/'receipt.json'))))
