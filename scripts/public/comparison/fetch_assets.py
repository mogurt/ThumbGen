"""Download the public comparison inputs and verify their checksums.
Existing files must match the published sizes and hashes.
"""
import argparse, hashlib, json, urllib.request
from pathlib import Path

def verify(path, row):
    if path.stat().st_size != row['bytes']:
        raise ValueError('Size mismatch: '+row['filename'])
    sha=hashlib.sha256(); blob=hashlib.sha1()
    blob.update(('blob '+str(row['bytes'])+'\0').encode())
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):
            sha.update(b);blob.update(b)
    if row.get('sha256') and sha.hexdigest()!=row['sha256']:
        raise ValueError('SHA-256 mismatch: '+row['filename'])
    if row.get('git_blob') and blob.hexdigest()!=row['git_blob']:
        raise ValueError('Pinned Git blob mismatch: '+row['filename'])
    return sha.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    lock=json.loads((Path(__file__).resolve().parents[3] / 'docs/performance/public-assets.json').read_text(encoding='utf-8'))
    receipt=[]
    for row in lock['license_files']+lock['files']:
        target=args.output/row['filename']
        if not target.exists():
            temp=target.with_name(target.name+'.part')
            if temp.exists():raise FileExistsError('Keep prior failed attempt; use fresh output: '+str(temp))
            req=urllib.request.Request(row['url'],headers={'User-Agent':'ThumbGen reproducible public benchmark'})
            with urllib.request.urlopen(req,timeout=90) as response,temp.open('xb') as f:
                while b:=response.read(1024*1024):f.write(b)
            verify(temp,{**row,'filename':row['filename']})
            temp.rename(target)
        receipt.append({**row,'verified_sha256':verify(target,row)})
        print('Verified '+row['filename'],flush=True)
    out=args.output/'verified-public-assets.json'
    if out.exists():raise FileExistsError(out)
    out.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
