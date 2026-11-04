"""Verify a Windows Conda SDK's package identities against its SHA256 explicit lock.

Archive verification is optional; installed file contents are not attested by metadata.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit,unquote


def digest(path):
    value=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):value.update(block)
    return value.hexdigest()


def verify(prefix,lock,archives):
    lines=[line.strip() for line in lock.read_text(encoding='utf-8-sig').splitlines() if line.strip() and not line.lstrip().startswith('#')]
    if not lines or lines[0]!='@EXPLICIT':raise ValueError('expected @EXPLICIT package lock')
    expected={}
    for line in lines[1:]:
        url=urlsplit(line);name=unquote(url.path.rsplit('/',1)[-1]);sha=url.fragment
        if url.scheme!='https' or url.username or url.password or url.query or not re.fullmatch('[0-9a-f]{64}',sha):
            raise ValueError('expected public HTTPS package URL with SHA256')
        if not name or name in expected:raise ValueError('duplicate or empty package filename')
        expected[name]=(url._replace(fragment='').geturl(),sha)
    installed={}
    for file in (prefix/'conda-meta').glob('*.json'):
        value=json.loads(file.read_text(encoding='utf-8'))
        name=value.get('fn')
        if not name or name in installed:raise ValueError('duplicate or missing installed package filename')
        installed[name]=value
    if set(installed)!=set(expected):raise ValueError('SDK package set differs: missing='+str(sorted(set(expected)-set(installed)))+' extra='+str(sorted(set(installed)-set(expected))))
    verified=[]
    for name,(url,sha) in expected.items():
        record=installed[name]
        if record.get('sha256')!=sha or record.get('url')!=url:raise ValueError('SDK identity mismatch: '+name)
        if archives:
            archive=next((root/name for root in archives if (root/name).is_file()),None)
            if archive is None or digest(archive)!=sha:raise ValueError('missing or mismatched package archive: '+name)
        verified.append(dict(name=record['name'],version=record['version'],build=record['build'],sha256=sha))
    return dict(schema_version=1,prefix=str(prefix.resolve()),lock_sha256=digest(lock),packages=len(verified),
                archive_content_verified=bool(archives),installed_file_contents_verified=False,identities=verified)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--lock',type=Path,default=Path(__file__).resolve().parents[1]/'dependencies/windows-sdk.explicit.txt')
    parser.add_argument('--archives',type=Path,action='append',default=[])
    parser.add_argument('--receipt',type=Path)
    args=parser.parse_args()
    try:result=verify(args.prefix,args.lock,args.archives)
    except (ValueError,OSError,KeyError) as error:parser.exit(1,str(error)+'\n')
    if args.receipt:
        args.receipt.parent.mkdir(parents=True,exist_ok=True)
        args.receipt.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='identities'}))
