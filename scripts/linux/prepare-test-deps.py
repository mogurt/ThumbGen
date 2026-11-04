"""Fetch verified test wheels on Windows, or install them offline on Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import sysconfig
import urllib.request
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--install', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
lock = json.loads((repo / 'scripts/linux/test-deps-lock.json').read_text())
cache = repo / '.deps/linux/test-wheels'
cache.mkdir(parents=True, exist_ok=True)
if args.install and (sys.platform != 'linux' or sys.version_info[:2] != (3, 11)):
    raise SystemExit('Offline test dependencies require Linux CPython 3.11')
for wheel in lock['wheels']:
    path = cache / wheel['filename']
    if not path.exists() and not args.install:
        with urllib.request.urlopen(wheel['url'], timeout=60) as response:
            path.write_bytes(response.read())
    if hashlib.sha256(path.read_bytes()).hexdigest() != wheel['sha256']:
        raise SystemExit('Wheel SHA256 mismatch: ' + path.name)
    if args.install:
        with zipfile.ZipFile(path) as archive:
            archive.extractall(sysconfig.get_path('purelib'))
    print(('Installed ' if args.install else 'Verified ') + path.name)
if args.install:
    from importlib.metadata import version
    for wheel in lock['wheels']:
        if version(wheel['name']) != wheel['version']:
            raise SystemExit('Installed test dependency version mismatch: ' + wheel['name'])
    import jsonschema
    jsonschema.Draft202012Validator.check_schema({'type': 'object'})
