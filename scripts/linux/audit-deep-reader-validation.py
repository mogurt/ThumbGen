"""Extend the frozen full-stage audit with the internal Deep file-reader test.

Budget refusal counts are measured from native output, never copied from another
platform. This audit does not claim public Deep conversion or a hard RSS bound.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stage', 'source', 'mainline', 'official', 'previous-stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expected-count', type=int, required=True)
    parser.add_argument('--sdk', type=Path)
    args = parser.parse_args()
    result = args.stage / 'results' / os.environ['WSL_DISTRO_NAME']
    destination = result / 'deep-reader-audit.json'
    if destination.exists():
        raise FileExistsError(destination)
    suite = json.loads((args.stage / 'tests/ctest-suite.json').read_text())['tests']
    assert len(suite) == args.expected_count
    assert 'core_deep_reader' in {test['name'] for test in suite}
    log = (result / 'Testing/Temporary/LastTest.log').read_text()
    block = log.split('Test: core_deep_reader\n', 1)[1].split('<end of output>', 1)[0]
    measured = re.search(r'(\d+) decoded Decimal references; (\d+) consecutive below-peak budget refusals; '
                         r'mixed scan/tile/alpha/cancellation/source/count-guard and independent threads passed', block)
    assert measured and int(measured[1]) == 117 and int(measured[2]) > 0
    command = [sys.executable, str(Path(__file__).with_name('audit-deep-kernel-validation.py'))]
    for key in ('stage', 'source', 'mainline', 'official', 'previous_stage', 'expected_count'):
        command += ['--' + key.replace('_', '-'), str(getattr(args, key))]
    if args.sdk:
        command += ['--sdk', str(args.sdk)]
    subprocess.run(command, check=True)
    summary = json.loads((result / 'deep-kernel-audit.json').read_text())
    summary.pop('internal_deep_kernel_only', None)
    summary.update(internal_deep_reader_only=True, decoded_decimal_references=int(measured[1]),
                   consecutive_budget_refusals=int(measured[2]), measured_fixture_peak_bytes=int(measured[2]) + 1,
                   native_test_sha256=hashlib.sha256((args.stage / 'bin/thumbgen_deep_reader_tests').read_bytes()).hexdigest(),
                   source_test_sha256=hashlib.sha256((args.source / 'tests/test_deep_reader.cpp').read_bytes()).hexdigest(),
                   limits='Internal reader/flattening only; public Deep conversion, general budgets, real assets, colour/background, hard RSS, performance and release remain open')
    destination.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: value for key, value in summary.items() if key != 'artifacts'}, indent=2))


if __name__ == '__main__':
    main()
