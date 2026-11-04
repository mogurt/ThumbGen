"""Audit frozen 201-frame / three-cycle serial and pool public-CLI reports."""
from collections import Counter
import hashlib
import json
import os
from pathlib import Path

from jsonschema import Draft202012Validator

repo = Path(__file__).resolve().parents[2]
stage = Path(os.environ['THUMBGEN_VALIDATION_DIR']).resolve()
validator = Draft202012Validator(json.loads((stage / 'schemas/protocol-v1.schema.json').read_text()))
corpus_path = Path(os.environ.get('THUMBGEN_SEQUENCE_CORPUS', repo / '.deps/bench/sequence201.json'))
corpus_bytes = corpus_path.read_bytes()
corpus = json.loads(corpus_bytes.decode('utf-8-sig'))
expected_ids = [f"{row['id']}-cycle-{cycle}" for cycle in range(3) for row in corpus]
runs = []
reference_hashes = None
reference_sources = None
overall_pins = set()
for distro in ('CentOS-7.9', 'rocky'):
    reports = sorted((stage / 'results' / distro).glob('sequence-*/report.json'))
    if len(reports) != 2:
        raise RuntimeError(f'{distro}: expected exactly two completed sequence batches')
    worker_counts = []
    for path in reports:
        report = json.loads(path.read_text())
        request = json.loads((path.parent / 'jobs.json').read_text())
        if [job['id'] for job in request['jobs']] != expected_ids:
            raise RuntimeError('Submitted sequence order differs from the frozen corpus/cycles')
        if {job['sequence_id'] for job in request['jobs']} != {row['sequence_id'] for row in corpus}:
            raise RuntimeError('Sequence identity differs from the corpus')
        if report['exit'] or not report['sources_unchanged'] or report['source_count'] != 201:
            raise RuntimeError('Sequence source or process validation failed')
        if report['output_count'] != 1206 or report['successful_outputs'] != 1206:
            raise RuntimeError('Expected 1206 successful outputs')
        rows = report['records']
        if len(rows) != 604 or len({row['id'] for row in rows[:-1]}) != 603:
            raise RuntimeError('Expected unique 603 task results and one summary')
        pins = set()
        states = Counter()
        frame_hashes = {}
        for row in rows:
            validator.validate(row)
            if row['type'] != 'result':
                continue
            if not row['ok'] or len(row['outputs']) != 2:
                raise RuntimeError('Sequence task failed')
            source = row['source']
            if (source['width'], source['height'], len(source['channels'])) != (1920, 1080, 16):
                raise RuntimeError('Unexpected source dimensions/channel inventory')
            execution = row['execution']
            if execution['threads'] != 1 or execution['estimate_model'] != 'buffers-v2':
                raise RuntimeError('Expected one-thread buffers-v2 task')
            if not 0 < execution['admission_mib'] < execution['estimated_task_mib']:
                raise RuntimeError('Incremental admission estimate was not separated')
            selection = row['selection']
            states[selection['lock_status']] += 1
            pins.add(json.dumps(selection['locked'], sort_keys=True))
            asset = row['id'].rsplit('-cycle-', 1)[0]
            hashes = [report['output_sha256'][Path(output['path']).name] for output in row['outputs']]
            if asset in frame_hashes and frame_hashes[asset] != hashes:
                raise RuntimeError('Output changed across sequence cycles')
            frame_hashes[asset] = hashes
        if dict(states) != {'created': 1, 'reused': 602} or len(pins) != 1 or len(frame_hashes) != 201:
            raise RuntimeError('Expected one immutable sequence pin with created=1/reused=602')
        overall_pins.update(pins)
        if reference_hashes is None:
            reference_hashes = report['output_sha256']
            reference_sources = report['source_sha256']
        elif reference_hashes != report['output_sha256'] or reference_sources != report['source_sha256']:
            raise RuntimeError('Serial/pool or platform output/source SHA mismatch')
        workers = rows[-1]['execution']['workers']
        worker_counts.append(workers)
        runs.append(dict(platform=distro, workers=workers, report=path.relative_to(stage).as_posix(),
                         tasks=603, outputs=1206, lock_status_counts=dict(states), unique_pins=1,
                         all_cycles_identical=True, schema_records=604, wall_seconds=report['wall_seconds'],
                         execution=rows[-1]['execution']))
    if sorted(worker_counts) != [1, 4]:
        raise RuntimeError('Expected serial and four-worker sequence batches')
if len(overall_pins) != 1:
    raise RuntimeError('Sequence pin changed between platforms or worker counts')
summary = dict(runs=runs, all_output_sha256_identical=True, source_sha256_unchanged=True, unique_pin_across_runs=1,
               corpus_sha256=hashlib.sha256(corpus_bytes).hexdigest(),
               unique_sources=201, total_tasks=2412, total_outputs=4824, total_schema_records=2416,
               limits='Existing WSL hosts; no RSS sampling, shared-storage or release-default conclusion')
(stage / 'sequence-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
(stage / 'sequence-corpus.json').write_bytes(corpus_bytes)
print(json.dumps(summary, indent=2))
