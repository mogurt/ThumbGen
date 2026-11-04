"""Exercise actual ENOSPC on an explicitly supplied, bounded private tmpfs."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--mount', required=True, type=Path)
parser.add_argument('--exe', required=True, type=Path)
parser.add_argument('--report', required=True, type=Path)
args = parser.parse_args()
mount = args.mount.resolve()
filesystem = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '--target', str(mount)], text=True).strip()
capacity = os.statvfs(mount)
if filesystem != 'tmpfs' or not mount.name.startswith('thumbgen-enospc.') or capacity.f_blocks * capacity.f_frsize > 8 * 1024 * 1024:
    raise SystemExit('Refusing to fill anything except the private tmpfs of at most 8 MiB')
if any(mount.iterdir()):
    raise SystemExit('Private tmpfs must start empty')
def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='thumbgen-enospc-input-') as directory:
    source = Path(directory) / 'source.png'
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    source.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 4, 2, 8, 2, 0, 0, 0)) +
                       chunk(b'IDAT', zlib.compress((b'\0' + b'\xff\0\0' * 4) * 2)) + chunk(b'IEND', b''))
    before_source = checksum(source)
    targets = {kind: mount / kind / ('preview.' + suffix) for kind, suffix in
               (('png', 'png'), ('jpeg', 'jpg'), ('webp', 'webp'))}
    for target in targets.values():
        target.parent.mkdir()
        target.write_bytes(b'old replace target must survive ENOSPC\n')
    originals = {kind: (checksum(target), target.stat().st_size) for kind, target in targets.items()}
    fill_errno = None
    with (mount / 'owned-filler').open('wb', buffering=0) as filler:
        try:
            for _ in range(129):
                filler.write(b'X' * 65536)
        except OSError as error:
            fill_errno = error.errno
    free_before = os.statvfs(mount).f_bavail * os.statvfs(mount).f_frsize
    if fill_errno != errno.ENOSPC or free_before != 0:
        raise SystemExit('Fixture did not reach real ENOSPC')
    cases = []
    for kind, target in targets.items():
        free_before = os.statvfs(mount).f_bavail * os.statvfs(mount).f_frsize
        if free_before != 0:
            raise SystemExit('Filesystem stopped being full before ' + kind)
        result = subprocess.run([str(args.exe.resolve()), 'convert', str(source), '--output', str(target),
                                 '--existing', 'replace', '--json'], capture_output=True, text=True, timeout=30)
        records = [json.loads(line) for line in result.stdout.splitlines() if line]
        temporary_entries = sorted(path.name for path in target.parent.iterdir() if path.name.startswith('.thumbgen-tmp-'))
        failures = []
        errors = [output.get('error', {}).get('code') for record in records for output in record.get('outputs', [])]
        errors += [record.get('error', {}).get('code') for record in records]
        if result.returncode != 4 or 'encode_failed' not in errors:
            failures.append('expected all-failed exit 4 with encode_failed')
        if checksum(source) != before_source:
            failures.append('input changed')
        before_target, before_target_size = originals[kind]
        if not target.exists() or checksum(target) != before_target:
            failures.append('old replace target changed')
        if temporary_entries:
            failures.append('owned temporary entries leaked')
        cases.append(dict(format=kind, free_bytes_before=free_before, exit=result.returncode,
                          records=records, stderr=result.stderr,
                          input_unchanged=checksum(source) == before_source,
                          old_target_unchanged=target.exists() and checksum(target) == before_target,
                          old_target_sha256=before_target,
                          target_sha256_after=checksum(target) if target.exists() else None,
                          target_bytes_before=before_target_size,
                          target_bytes_after=target.stat().st_size if target.exists() else None,
                          temporary_entries=temporary_entries, failures=failures))
    failures = [case['format'] + ': ' + failure for case in cases for failure in case['failures']]
    report = dict(filesystem=filesystem, capacity_bytes=capacity.f_blocks * capacity.f_frsize,
                  filling_errno=fill_errno, user_uid=os.getuid(), input_sha256=before_source,
                  input_unchanged=all(case['input_unchanged'] for case in cases),
                  old_targets_unchanged=all(case['old_target_unchanged'] for case in cases),
                  cases=cases, failures=failures)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    raise SystemExit(bool(failures))
