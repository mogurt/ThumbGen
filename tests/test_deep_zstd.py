"""Run rejection cases in distinct processes; preserve raw stdout/stderr and exit codes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--exe', required=True)
p.add_argument('--runtime-dir', required=True)
p.add_argument('--report-dir', type=Path)
p.add_argument('--product-exe')
p.add_argument('--schema-dir', type=Path)
a = p.parse_args()
root = a.report_dir or Path.cwd() / ('deep-zstd-results-' + str(time.time_ns()))
root.mkdir(parents=True)
env = dict(os.environ)
key = 'PATH' if os.name == 'nt' else 'LD_LIBRARY_PATH'
env[key] = a.runtime_dir + os.pathsep + env.get(key, '')
records = []
def run(name, *args, exe=None, expected_exit=0):
    command = [exe or a.exe, *map(str, args)]
    result = subprocess.run(command, env=env, capture_output=True, timeout=120)
    (root / (name + '.stdout')).write_bytes(result.stdout)
    (root / (name + '.stderr')).write_bytes(result.stderr)
    records.append(dict(name=name, command=command, exit=result.returncode))
    (root / 'processes.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print(name, 'exit', result.returncode, flush=True)
    if result.returncode != expected_exit:
        raise RuntimeError(f'{name} failed; unmodified child output in {root}: {result.stderr!r}')
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    return rows[0] if len(rows) == 1 else rows
report = run('success')
if report['bounded']:
    for index in range(1, report['fault_allocations'] + 1):
        run(f'fault-{index:03}', 'fault', index)
    for kind in ('fds-count', 'fds-sample', 'fds-zero', 'wire-version', 'wire-flags', 'payload-size', 'truncated-frame', 'declared-size'):
        run(kind, 'corrupt', kind)
if a.product_exe:
    from jsonschema import Draft202012Validator
    sys.path.insert(0, str(Path(__file__).parent / 'qa'))
    from alpha_reference import read_png
    validator = Draft202012Validator(json.loads((a.schema_dir / 'protocol-v1.schema.json').read_text(encoding='utf-8')))
    def cli(name, *args, expected_exit=0):
        result = run(name, *args, '--json', exe=a.product_exe, expected_exit=expected_exit)
        rows = result if isinstance(result, list) else [result]
        for row in rows:
            validator.validate(row)
        return rows
    caps = cli('capabilities', 'capabilities')[0]
    assert ('zstd' in caps['deep_compressions']) == report['bounded']
    inputs = []
    for layout in ('scan', 'tile'):
        source = (root / (layout + '.exr')).resolve()
        run('fixture-' + layout, 'fixture', source, layout)
        inputs.append(source)
        output = (root / (layout + '.png')).resolve()
        rows = cli('convert-' + layout, 'convert', source, '--output', output, '--deep-mode', 'flatten',
                   '--threads', '1', '--input-space', 'linear', '--ocio-config', 'ocio://default',
                   '--size', '129x9', '--preset', 'fast', '--background', 'black', expected_exit=0 if report['bounded'] else 4)
        if report['bounded']:
            w, h, bands, pixels = read_png(output)
            assert (w, h, bands) == (129, 9, 3)
            def srgb(v):
                return round(255 * (12.92 * v if v <= .0031308 else 1.055 * v ** (1 / 2.4) - .055))
            for y in range(9):
                expected = (0, srgb(.75), srgb(.5)) if y % 4 == 1 else (srgb(.5), srgb(.8125), srgb(.5)) if y % 4 == 2 else (0, 0, 0)
                for x in range(129):
                    assert all(abs(v - e) <= 1 for v, e in zip(pixels[y * 129 + x], expected))
        else:
            assert rows[0]['error']['code'] == 'deep_layout_unsupported'
            assert not output.exists()
    if report['bounded']:
        # Same Deep mapping stays pinned through an actual decode-limit failure.
        jobs = [dict(id=str(i), sequence_id='zstd-shot', input=str(inputs[i % 2]),
                     outputs=[dict(path=str((root / f'batch-{i}.png').resolve()))],
                     options=dict(deep_samples_per_pixel=1) if i == 1 else {}) for i in range(3)]
        manifest = dict(schema_version=1, defaults=dict(deep_mode='flatten', ocio_config='ocio://default',
                        input_space='linear', threads=1, size=[129, 9], preset='fast'), jobs=jobs)
        path = root / 'jobs.json'
        path.write_text(json.dumps(manifest), encoding='utf-8')
        rows = cli('batch-recovery', 'batch', '--manifest', path.resolve(), expected_exit=3)
        results = [r for r in rows if r['type'] == 'result']
        assert [r['status'] for r in results] == ['success', 'failed', 'success']
        assert results[1]['error']['code'] == 'deep_sample_limit_exceeded'
        assert results[2]['selection']['lock_status'] == 'reused'
        assert len({json.dumps(r['selection']['locked'], sort_keys=True) for r in results}) == 1
        assert read_png(root / 'batch-0.png') == read_png(root / 'batch-2.png')
    report['public_zstd'] = True
(root / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(dict(results=str(root), **report)))
