"""Decode every successful output of a frozen public benchmark using the native helper."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('report', 'helper', 'runtime-dir', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expected-count', type=int, required=True)
    args = parser.parse_args()
    if args.output.exists(): raise FileExistsError(args.output)
    report = json.loads(args.report.read_text())
    expected = [output for run in report['runs'] + report['boundaries'] for record in run['records']
                if record['type'] == 'result' for output in record['outputs'] if output['status'] == 'success']
    assert len(expected) == args.expected_count
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.runtime_dir))
    checks = []
    for output in expected:
        process = subprocess.run([str(args.helper), output['path']], env=env, capture_output=True, text=True)
        pixels = json.loads(process.stdout)
        check = dict(path=output['path'], sha256=digest(output['path']), pixels=pixels, exit=process.returncode,
                     reported_size=[output['width'], output['height']])
        checks.append(check)
        if process.returncode or not pixels.get('readable') or [pixels.get('width'), pixels.get('height')] != check['reported_size']:
            args.output.write_text(json.dumps(dict(failed=True, checks=checks, stderr=process.stderr), indent=2)+'\n')
            raise AssertionError('Native benchmark read-back failed: ' + output['path'])
    result = dict(benchmark_report_sha256=digest(args.report), helper_sha256=digest(args.helper),
                  readbacks=len(checks), checks=checks, script_sha256=digest(__file__))
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(readbacks=len(checks), benchmark_report_sha256=result['benchmark_report_sha256'])))


if __name__ == '__main__': main()
