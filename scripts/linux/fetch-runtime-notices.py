"""Fetch the signed CentOS runtime RPM used only as a source of license texts."""
import hashlib
from pathlib import Path
import urllib.request

url = 'https://vault.centos.org/7.9.2009/os/x86_64/Packages/libgcc-4.8.5-44.el7.x86_64.rpm'
expected = 'cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa'
directory = Path(__file__).resolve().parents[2] / '.deps/linux/license-rpms'
directory.mkdir(parents=True, exist_ok=True)
path = directory / url.rsplit('/', 1)[-1]
if not path.exists():
    with urllib.request.urlopen(url, timeout=60) as response:
        path.write_bytes(response.read())
if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
    raise SystemExit('Runtime notice RPM digest mismatch; file left for inspection')
print(path)
