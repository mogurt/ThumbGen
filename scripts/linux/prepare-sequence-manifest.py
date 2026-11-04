"""Make the fixed 201-frame, three-cycle public batch request without reading assets."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('corpus', type=Path)
parser.add_argument('destination', type=Path)
parser.add_argument('--workers', type=int, choices=(1, 4), required=True)
args = parser.parse_args()
rows = json.loads(args.corpus.read_text(encoding='utf-8-sig'))
if len(rows) != 201 or len({row['id'] for row in rows}) != 201:
    raise SystemExit('Expected 201 unique frame IDs')
if [row['frame'] for row in rows] != sorted({row['frame'] for row in rows}):
    raise SystemExit('Expected unique ascending frame order')
if len({row['sequence_id'] for row in rows}) != 1 or not rows[0]['sequence_id']:
    raise SystemExit('Expected one explicit nonempty sequence identity')
manifest = dict(schema_version=1, defaults=dict(threads=1, preset='balanced', quality=90),
                execution=dict(workers=args.workers, thread_budget=8, memory_budget_mib=8192),
                jobs=[dict(id=f"{row['id']}-cycle-{cycle}", input=row['path'], sequence_id=row['sequence_id'],
                           outputs=[dict(path=f'preview-{cycle}-{row["id"]}-{index}.jpg', size=size, format='jpeg')
                                    for index, size in enumerate(([960, 960], [425, 260]))])
                      for cycle in range(3) for row in rows])
args.destination.parent.mkdir(parents=True, exist_ok=True)
with args.destination.open('x', encoding='utf-8') as output:
    json.dump(manifest, output, indent=2)
    output.write('\n')
print(args.destination)
