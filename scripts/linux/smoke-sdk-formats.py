"""Exercise four synthetic input formats and three output encoders on a staged SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

from jsonschema import Draft202012Validator

parser = argparse.ArgumentParser()
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--exe', type=Path)
args = parser.parse_args()
stage = args.stage.resolve()
binary = args.exe.resolve() if args.exe else stage / 'bin/thumbgen'
destination = stage / 'results' / os.environ.get('WSL_DISTRO_NAME', 'linux') / ('sdk-smoke-' + uuid.uuid4().hex[:8])
destination.mkdir(parents=True)
sources = [stage / 'fixtures' / ('synthetic.' + suffix) for suffix in ('png', 'exr', 'jpg', 'webp')]
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
before = {path.name: digest(path) for path in sources}
manifest = dict(schema_version=1, defaults=dict(threads=1, preset='balanced', quality=90),
                jobs=[dict(id=path.suffix[1:], input=str(path),
                           outputs=[dict(path=str(destination / (path.suffix[1:] + '-to.' + suffix)),
                                         size=[32, 16], format=kind)
                                    for suffix, kind in (('png','png'), ('jpg','jpeg'), ('webp','webp'))])
                      for path in sources])
request = destination / 'jobs.json'
request.write_text(json.dumps(manifest, indent=2) + '\n')
environment = dict(os.environ, LD_LIBRARY_PATH=str(stage / 'lib'), OCIO='ocio://default')
result = subprocess.run([str(binary), 'batch', '--manifest', str(request), '--json'],
                        env=environment, capture_output=True, text=True, timeout=120)
rows = [json.loads(line) for line in result.stdout.splitlines() if line]
validator = Draft202012Validator(json.loads((stage / 'schemas/protocol-v1.schema.json').read_text()))
for row in rows:
    validator.validate(row)
outputs = [output for row in rows for output in row.get('outputs', [])]
report = dict(executable=str(binary), executable_sha256=digest(binary), exit=result.returncode, stderr=result.stderr,
              records=rows, source_sha256=before, sources_unchanged=before == {path.name: digest(path) for path in sources},
              output_sha256={Path(output['path']).name: digest(Path(output['path'])) for output in outputs if output['status']=='success'},
              schema_valid=True, decoded_outputs=0)
(destination / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
if result.returncode or len(outputs)!=12 or any(output['status']!='success' for output in outputs) or not report['sources_unchanged']:
    raise RuntimeError('Expected four synthetic inputs and twelve successful outputs')
for output in outputs:
    inspected = subprocess.run([str(binary), 'inspect', output['path'], '--json'], env=environment,
                               capture_output=True, text=True, timeout=30)
    (destination / (Path(output['path']).name + '.inspection.json')).write_text(inspected.stdout)
    if inspected.returncode:
        raise RuntimeError('Encoded output failed public CLI decode inspection: ' + output['path'])
report['decoded_outputs'] = len(outputs)
(destination / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(destination)
