"""Save real-signal records using the frozen test helper that owns both CLI PIDs."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zlib

from jsonschema import Draft202012Validator

if sys.platform != 'linux':
    raise SystemExit('This diagnostic records Linux SIGINT/SIGTERM only')
repo = Path(__file__).resolve().parents[2]
stage = Path(os.environ.get('THUMBGEN_VALIDATION_DIR', repo / '.deps/linux/validation')).resolve()
destination = stage / 'results' / os.environ.get('WSL_DISTRO_NAME', 'linux') / ('interrupt-' + uuid.uuid4().hex[:8])
destination.mkdir(parents=True)
validator = Draft202012Validator(json.loads((stage / 'schemas/protocol-v1.schema.json').read_text()))
environment = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join((str(stage / 'lib'), str(Path(sys.prefix) / 'lib'))),
                   OCIO='ocio://default')
reports = {}


def chunk(kind, payload):
    return struct.pack('!I', len(payload)) + kind + payload + struct.pack('!I', zlib.crc32(kind + payload))


for kind in ('int', 'term'):
    root = destination / kind
    root.mkdir()
    source = root / 'source.png'
    source.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 2, 1, 8, 2, 0, 0, 0)) +
                       chunk(b'IDAT', zlib.compress(b'\0\xff\0\0\xff\0\0')) + chunk(b'IEND', b''))
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    jobs = [dict(id=str(index), input=str(source), outputs=[dict(path=str(root / f'{index}.png'))])
            for index in range(256)]
    manifest = root / 'jobs.json'
    manifest.write_text(json.dumps(dict(schema_version=1, execution=dict(workers=2, thread_budget=2,
                              memory_budget_mib=1024), jobs=jobs), indent=2) + '\n')
    result = subprocess.run([sys.executable, str(stage / 'tests/test_interrupt.py'), '--helper',
                             str(stage / 'bin/thumbgen'), str(manifest), kind], env=environment,
                            capture_output=True, text=True, timeout=45)
    (root / 'helper-stdout.json').write_text(result.stdout)
    (root / 'helper-stderr.txt').write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{kind}: frozen helper failed ({result.returncode}): {result.stderr}')
    data = json.loads(result.stdout)
    rows = data['records']
    if data['exit'] != 130 or len(rows) != 257:
        raise RuntimeError('Expected cancellation exit 130 with 256 results and summary')
    for row in rows:
        validator.validate(row)
    summary = rows[-1]
    if not summary['interrupted'] or summary['cancelled'] < 1 or summary['success'] < 1:
        raise RuntimeError('Expected both committed successes and cancellation')
    if {row['id'] for row in rows[:-1]} != {job['id'] for job in jobs}:
        raise RuntimeError('Missing or unexpected task IDs')
    if len({row['id'] for row in rows[:-1]}) != 256:
        raise RuntimeError('Duplicate task IDs')
    if summary['total'] != sum(summary[key] for key in ('success','partial','failed','skipped','cancelled')):
        raise RuntimeError('Invalid task summary arithmetic')
    outputs = [output for row in rows[:-1] for output in row['outputs']]
    for state in ('success', 'failed', 'skipped', 'cancelled'):
        if summary['output_counts'][state] != sum(output['status'] == state for output in outputs):
            raise RuntimeError('Invalid output summary arithmetic')
    for output in outputs:
        path = Path(output['path'])
        if output['status'] == 'success' and not path.read_bytes().startswith(b'\x89PNG'):
            raise RuntimeError('Committed target missing or invalid')
        if output['status'] == 'cancelled' and path.exists():
            raise RuntimeError('Cancelled target exists')
    if hashlib.sha256(source.read_bytes()).hexdigest() != before or list(root.rglob('.thumbgen-tmp-*')):
        raise RuntimeError('Source changed or temporary file remains')
    reports[kind] = dict(helper_exit=0, cli_exit=130, signals_sent=2, records=len(rows), schema_valid=True,
                         source_sha256=before, source_unchanged=True, no_temporary_files=True,
                         independent_peer_survived_helper_check=True, summary=summary)
(destination / 'report.json').write_text(json.dumps(reports, indent=2) + '\n')
print(destination)
