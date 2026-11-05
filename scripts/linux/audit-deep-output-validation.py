"""Audit internal associated RGBA/background output and the complete frozen stage.

Regenerate the independent Decimal background oracle from frozen source only;
never use the tested reader or CLI to manufacture expected values.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stage', 'source', 'mainline', 'official', 'previous-stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expected-count', type=int, required=True)
    parser.add_argument('--sdk', type=Path)
    args = parser.parse_args()
    result = args.stage / 'results' / os.environ['WSL_DISTRO_NAME']
    destination = result / 'deep-output-audit.json'
    if destination.exists():
        raise FileExistsError(destination)
    suite = json.loads((args.stage / 'tests/ctest-suite.json').read_text())['tests']
    assert len(suite) == args.expected_count and 'core_deep_output' in {row['name'] for row in suite}
    block = (result / 'Testing/Temporary/LastTest.log').read_text().split('Test: core_deep_output\n', 1)[1].split('<end of output>', 1)[0]
    assert '468 Decimal100 opaque background comparisons; stable scalar-alpha RGBA, HDR transmission, budget and emission guards passed' in block
    independent = result / 'independent-background-reference'
    independent.mkdir()
    regenerated = independent / 'deep-background-reference-v1.json'
    generator = args.source / 'tests/generate_deep_background_reference.py'
    frozen = args.source / 'tests/data/deep-background-reference-v1.json'
    execution = subprocess.run([sys.executable, str(generator), '--output', str(regenerated)], text=True, capture_output=True)
    (independent / 'stdout.log').write_text(execution.stdout)
    (independent / 'stderr.log').write_text(execution.stderr)
    (independent / 'exit-code.txt').write_text(str(execution.returncode) + '\n')
    execution.check_returncode()
    assert frozen.read_bytes() == regenerated.read_bytes()
    reference = json.loads(frozen.read_text())
    assert len(reference['cases']) == 117 and len(reference['backgrounds']) == 4 and reference['precision'] == 100
    assert reference['source_reference_sha256'] == digest(args.source / 'tests/data/deep-reference-v1.json')
    assert all(len(row['expected']) == 4 for row in reference['cases'])
    command = [sys.executable, str(Path(__file__).with_name('audit-deep-reader-validation.py'))]
    for key in ('stage', 'source', 'mainline', 'official', 'previous_stage', 'expected_count'):
        command += ['--' + key.replace('_', '-'), str(getattr(args, key))]
    if args.sdk:
        command += ['--sdk', str(args.sdk)]
    subprocess.run(command, check=True)
    summary = json.loads((result / 'deep-reader-audit.json').read_text())
    summary.update(background_comparisons=468, background_reference_cases=117, backgrounds=reference['backgrounds'],
                   output_test_sha256=digest(args.stage / 'bin/thumbgen_deep_output_tests'),
                   output_test_source_sha256=digest(args.source / 'tests/test_deep_output.cpp'),
                   independent_background_reference=dict(generator_sha256=digest(generator),
                       source_basis_sha256=reference['source_reference_sha256'], frozen_sha256=digest(frozen),
                       regenerated_sha256=digest(regenerated), precision=100, byte_equal=True),
                   rgba_semantics='Associated RGB and scalar alpha; structural alpha-name policy, not per-frame numerical equality',
                   limits='Internal associated RGBA/background output only; public CLI Deep, production support, real renderer assets, hard RSS, performance and release remain open')
    destination.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: value for key, value in summary.items() if key != 'artifacts'}, indent=2))


if __name__ == '__main__':
    main()
