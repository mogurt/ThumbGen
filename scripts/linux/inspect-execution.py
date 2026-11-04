"""Record public execution quotas and estimates from a frozen validation stage.

Uses a generated PNG only; jsonschema is a test dependency, never a product library.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

from jsonschema import Draft202012Validator

repo = Path(__file__).resolve().parents[2]
stage = Path(os.environ.get('THUMBGEN_VALIDATION_DIR', repo / '.deps/linux/validation')).resolve()
label = os.environ.get('WSL_DISTRO_NAME', 'linux')
destination = stage / 'results' / label / ('execution-' + uuid.uuid4().hex[:8])
destination.mkdir(parents=True)


def chunk(kind, payload):
    return struct.pack('!I', len(payload)) + kind + payload + struct.pack('!I', zlib.crc32(kind + payload))


source = destination / 'synthetic.png'
source.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 16, 16, 8, 2, 0, 0, 0)) +
                   chunk(b'IDAT', zlib.compress((b'\0' + b'\x30\x80\xc0' * 16) * 16)) + chunk(b'IEND', b''))
before = hashlib.sha256(source.read_bytes()).hexdigest()
validator = Draft202012Validator(json.loads((stage / 'schemas/protocol-v1.schema.json').read_text()))
environment = dict(os.environ, LD_LIBRARY_PATH=str(stage / 'lib'))
captures = {}


def capture(name, arguments, expected):
    result = subprocess.run([str(stage / 'bin/thumbgen'), *map(str, arguments), '--json'],
                            env=environment, capture_output=True, text=True, timeout=60)
    records = [json.loads(line) for line in result.stdout.splitlines() if line]
    captures[name] = dict(exit=result.returncode, stderr=result.stderr, records=records)
    (destination / 'report.json').write_text(json.dumps(captures, indent=2) + '\n')
    if result.returncode != expected:
        raise RuntimeError(f'{name}: expected exit {expected}, got {result.returncode}: {result.stderr}')
    for record in records:
        validator.validate(record)
    return records


rows = capture('convert', ['convert', source, '--output', destination / 'one.png', '--threads', 8,
                         '--thread-budget', 1, '--memory-budget-mib', 512,
                         '--startup-timeout-ms', 10000, '--task-timeout-ms', 10000], 0)
execution = rows[0]['execution']
if (execution['requested_threads'], execution['threads'], execution['thread_budget']) != (8, 1, 1):
    raise RuntimeError('Unexpected effective convert thread quota')
manifest = dict(schema_version=1, execution=dict(thread_budget=2, memory_budget_mib=512,
                startup_timeout_ms=10000, task_timeout_ms=10000), jobs=[
    dict(id=f'quota-{threads}', input=str(source), options=dict(threads=threads),
         outputs=[dict(path=str(destination / f'batch-{threads}.png'), format='png', size=16)])
    for threads in (8, 4)])
path = destination / 'jobs.json'
Draft202012Validator(json.loads((stage / 'schemas/jobs-v1.schema.json').read_text())).validate(manifest)
path.write_text(json.dumps(manifest, indent=2) + '\n')
rows = capture('batch', ['batch', '--manifest', path], 0)
if [row['execution']['threads'] for row in rows[:-1]] != [2, 2]:
    raise RuntimeError('Unexpected effective batch thread quotas')
rows = capture('refused', ['convert', source, '--output', destination / 'refused.png',
                         '--memory-budget-mib', 129], 4)
if rows[0]['error']['code'] != 'task_memory_exceeds_budget' or (destination / 'refused.png').exists():
    raise RuntimeError('Expected memory refusal with no output')
if captures['convert']['records'][0]['execution'].get('estimate_model') == 'buffers-v2':
    rows = capture('v2-small-budget', ['convert', source, '--output', destination / 'incremental.png',
                                     '--thread-budget', 1, '--memory-budget-mib', 266], 0)
    execution = rows[0]['execution']
    if (execution['estimate_model'] != 'buffers-v2' or execution['admission_mib'] >= 2 or
            execution['fixed_reserve_mib'] + execution['admission_mib'] > 266):
        raise RuntimeError('Small image did not fit incremental admission budget')
jobs_schema = json.loads((stage / 'schemas/jobs-v1.schema.json').read_text())
if 'workers' in jobs_schema['$defs']['execution']['properties']:
    for workers in (1, 2, 4, 8):
        pool = dict(schema_version=1, execution=dict(workers=workers, thread_budget=8, memory_budget_mib=8192),
                    jobs=[dict(id=f'pool-{workers}-{index}', input=str(source), options=dict(threads=8),
                               outputs=[dict(path=str(destination / f'pool-{workers}-{index}.png'), size=16)])
                          for index in range(8)])
        Draft202012Validator(jobs_schema).validate(pool)
        path = destination / f'pool-{workers}.json'
        path.write_text(json.dumps(pool, indent=2) + '\n')
        rows = capture(f'pool-{workers}', ['batch', '--manifest', path], 0)
        configuration = rows[-1]['execution']
        quota = 8 // workers
        if (configuration['requested_workers'], configuration['workers']) != (workers, workers):
            raise RuntimeError('Unexpected worker allocation')
        if configuration['fixed_reserve_mib'] != 128 + workers * (128 + 8 * quota):
            raise RuntimeError('Unexpected pool fixed reserve')
        tasks = rows[:-1]
        if len(tasks) != 8 or any(not row['ok'] or row['execution']['threads'] != quota or
                                  row['execution']['queue_ms'] < 0 for row in tasks):
            raise RuntimeError('Unexpected pool task allocation or queue diagnostics')
if hashlib.sha256(source.read_bytes()).hexdigest() != before:
    raise RuntimeError('Synthetic input changed')
captures['source_sha256'] = before
captures['source_unchanged'] = True
captures['schema_valid'] = True
(destination / 'report.json').write_text(json.dumps(captures, indent=2) + '\n')
print(destination)
