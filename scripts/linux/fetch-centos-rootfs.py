"""Fetch and verify the pinned official single-layer CentOS rootfs (Windows Python)."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request

OUT = Path(__file__).resolve().parents[2] / '.deps/linux/rootfs'
INDEX = 'sha256:be65f488b7764ad3638f236b7b515b3678369a5124c47b8d32916d6487418ea4'
IMAGE = 'sha256:dead07b4d8ed7e29e98de0f4504d87e8880d4347859d839686a31da35a3b532f'
ROOTFS = '174f5685490326fc0a1c0f5570b8663732189b327007e47ff13d2ca59673db02'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--verify-cache', action='store_true', help='Verify existing cached files offline, without writing or downloading')
args = parser.parse_args()
if not args.verify_cache:
    OUT.mkdir(parents=True, exist_ok=True)
    token = json.load(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/centos:pull', timeout=30))['token']

def fetch(kind, identity):
    if args.verify_cache:
        name = 'layer.tar.gz' if kind == 'blobs' else ('manifest.json' if identity == IMAGE else 'index.json')
        data = (OUT / name).read_bytes()
    else:
        req = urllib.request.Request('https://registry-1.docker.io/v2/library/centos/' + kind + '/' + identity,
            headers={'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.docker.distribution.manifest.v2+json'})
        with urllib.request.urlopen(req, timeout=60) as response:
            data = response.read()
    actual = 'sha256:' + hashlib.sha256(data).hexdigest()
    if identity.startswith('sha256:') and actual != identity:
        raise RuntimeError('Digest mismatch: ' + identity)
    return data, actual

index, digest = fetch('manifests', 'centos7.9.2009')
if digest != INDEX:
    raise RuntimeError('Manifest index digest mismatch: ' + digest)
if not any(m['digest'] == IMAGE and m['platform']['architecture'] == 'amd64' and m['platform']['os'] == 'linux' for m in json.loads(index)['manifests']):
    raise RuntimeError('Pinned linux/amd64 image is missing from manifest index')
manifest, _ = fetch('manifests', IMAGE)
image = json.loads(manifest)
if len(image['layers']) != 1:
    raise RuntimeError('Multi-layer image requires overlay/whiteout processing; refusing concatenation')
layer = image['layers'][0]['digest']
blob, _ = fetch('blobs', layer)
if not args.verify_cache:
    (OUT / 'layer.tar.gz').write_bytes(blob)
    with gzip.open(OUT / 'layer.tar.gz', 'rb') as source, (OUT / 'rootfs.tar').open('wb') as target:
        shutil.copyfileobj(source, target)
rootfs_digest = hashlib.sha256((OUT / 'rootfs.tar').read_bytes()).hexdigest()
if rootfs_digest != ROOTFS:
    raise RuntimeError('Uncompressed rootfs digest mismatch: ' + rootfs_digest)
with tarfile.open(OUT / 'rootfs.tar') as archive:
    if any('/.wh.' in '/' + m.name for m in archive):
        raise RuntimeError('Unexpected whiteouts')
    release_file = archive.extractfile('etc/centos-release')
    if release_file is None:
        raise RuntimeError('CentOS release file is not a regular file')
    release = release_file.read().decode().strip()
if release != 'CentOS Linux release 7.9.2009 (Core)':
    raise RuntimeError('Unexpected CentOS release: ' + release)
record = dict(tag='library/centos:centos7.9.2009', index=INDEX, image=IMAGE, layer=layer,
              rootfs_sha256=rootfs_digest, release=release)
if not args.verify_cache:
    (OUT / 'identity.json').write_text(json.dumps(record, indent=2) + '\n')
    (OUT / 'index.json').write_bytes(index)
    (OUT / 'manifest.json').write_bytes(manifest)
print(json.dumps(record, indent=2))
