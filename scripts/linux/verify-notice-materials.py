"""Inventory fixed-RPM notice materials; existing receipts are verified, never rewritten."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--create', action='store_true')
args = parser.parse_args()
root = args.directory.resolve()
receipt = root / 'NOTICE-MATERIALS.json'
files = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
         for path in sorted(root.rglob('*')) if path.is_file() and path != receipt}
expected = 'cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa'
if args.create:
    with receipt.open('x') as output:
        json.dump(dict(rpm='libgcc-4.8.5-44.el7.x86_64.rpm', rpm_sha256=expected,
                       source='CentOS Vault 7.9.2009; rpm -K verified by extraction entry', files=files), output, indent=2)
        output.write('\n')
else:
    report = json.loads(receipt.read_text())
    if report['rpm_sha256'] != expected or report['files'] != files:
        raise SystemExit('Notice material contents differ; existing directory left unchanged')
print('Verified notice materials:', len(files))
