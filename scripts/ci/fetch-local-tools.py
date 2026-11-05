"""Download hash-locked optional Linux validation tools into this worktree only."""
import hashlib
import json
from pathlib import Path
import urllib.request


repo = Path(__file__).resolve().parents[2]
lock = json.loads((repo / 'scripts/ci/linux-local-tools.json').read_text(encoding='utf-8-sig'))
downloads = repo / '.deps/ci-linux-tools/downloads'
downloads.mkdir(parents=True, exist_ok=True)
for name, item in lock.items():
    path = downloads / item['filename']
    if not path.exists():
        urllib.request.urlretrieve(item['url'], path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
        raise RuntimeError('Tool archive hash mismatch: ' + name)
    print(name, item['version'], item['sha256'])
