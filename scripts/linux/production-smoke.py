"""Run an explicitly supplied existing manifest read-only, redirecting all outputs.

Private asset paths/results stay under .deps; no samples are copied or uploaded.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--manifest', required=True, type=Path)
parser.add_argument('--asset-root', required=True, type=Path)
parser.add_argument('--readonly-root', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
stage = Path(os.environ.get('THUMBGEN_VALIDATION_DIR', repo / '.deps/linux/validation')).resolve()
label = os.environ.get('WSL_DISTRO_NAME', 'linux')
destination = stage / 'results' / label / ('production-' + uuid.uuid4().hex[:8])
destination.mkdir(parents=True)
manifest = json.loads(args.manifest.read_text(encoding='utf-8-sig'))
def linux_path(value):
    if len(value) > 2 and value[1:3] in (':\\', ':/'):
        return Path('/mnt') / value[0].lower() / value[3:].replace('\\', '/')
    path = Path(value)
    return path if path.is_absolute() else args.manifest.resolve().parent / path
def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            checksum.update(chunk)
    return checksum.hexdigest()
before = {}
for index, job in enumerate(manifest['jobs']):
    original = linux_path(job['input']).resolve()
    source = args.readonly_root.resolve() / original.relative_to(args.asset_root.resolve())
    mount_options = subprocess.check_output(['findmnt', '-n', '-o', 'OPTIONS', '--target', str(source)], text=True)
    if 'ro' not in mount_options.strip().split(','):
        raise SystemExit('Asset mount must be read-only')
    before[str(source)] = digest(source)
    job['input'] = str(source)
    for output_index, output in enumerate(job['outputs']):
        suffix = Path(output['path']).suffix
        output['path'] = str(destination / f'{index:03}-{output_index}{suffix}')
    if 'ocio_config' in job.get('options', {}) and job['options']['ocio_config']:
        job['options']['ocio_config'] = str(linux_path(job['options']['ocio_config']))
path = destination / 'jobs.json'
path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
environment = dict(os.environ, LD_LIBRARY_PATH=str(stage / 'lib'))
result = subprocess.run([str(stage / 'bin/thumbgen'), 'batch', '--manifest', str(path), '--json'],
                        env=environment, capture_output=True, text=True, timeout=180)
after = {source: digest(Path(source)) for source in before}
records = [json.loads(line) for line in result.stdout.splitlines() if line]
outputs = [output for record in records for output in record.get('outputs', [])]
report = dict(exit=result.returncode, source_count=len(before), sources_unchanged=before == after,
              source_sha256=before, output_count=len(outputs),
              successful_outputs=sum(output.get('status') == 'success' for output in outputs),
              stderr=result.stderr, records=records)
(destination / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
print(json.dumps({key: report[key] for key in ('exit', 'source_count', 'sources_unchanged', 'output_count', 'successful_outputs')}))
print(destination)
raise SystemExit(result.returncode if before == after else 1)
