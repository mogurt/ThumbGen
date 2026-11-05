"""Audit the internal Deep kernel without claiming public Deep conversion support.

The common audit reopens every production output and verifies the frozen source,
runtime, schema receipts, independent Cryptomatte oracle and previous-stage chain.
This extension regenerates the Decimal oracle in a fresh directory and checks the
new native test's complete CTest output. It never creates expected pixels using
the program under test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


REFERENCE_SHA256 = 'a0eb4ce9f06849c5a6882a8f7ffdcab48c5bc8aeb413ee1462b0d5926e6eba85'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stage', 'source', 'mainline', 'official', 'previous-stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expected-count', type=int, required=True)
    parser.add_argument('--sdk', type=Path)
    args = parser.parse_args()
    stage = args.stage.resolve()
    result = stage / 'results' / os.environ['WSL_DISTRO_NAME']
    destination = result / 'deep-kernel-audit.json'
    if destination.exists():
        raise FileExistsError(destination)
    load = lambda path: json.loads(path.read_text())
    suite = load(stage / 'tests/ctest-suite.json')['tests']
    assert len(suite) == args.expected_count
    assert {'core_deep_flatten', 'core_sampled_id', 'public_sampled_id'}.issubset({t['name'] for t in suite})
    log = (result / 'Testing/Temporary/LastTest.log').read_text()
    block = log.split('Test: core_deep_flatten\n', 1)[1].split('<end of output>', 1)[0]
    assert '117 Decimal references and 1404 exact permutation checks; OIIO raw-flatten limits and resource/invalid-value guards passed' in block
    assert 'Ran 7 tests' in log.split('Test: public_sampled_id\n', 1)[1].split('<end of output>', 1)[0]
    reference = args.source / 'tests/data/deep-reference-v1.json'
    assert digest(reference) == REFERENCE_SHA256
    oracle = load(reference)
    assert len(oracle['cases']) == 117 and oracle['precision'] == 100 and oracle['seed'] == 94198
    independent = result / 'independent-decimal-reference'
    independent.mkdir()
    regenerated = independent / 'deep-reference-v1.json'
    generator = args.source / 'tests/generate_deep_reference.py'
    execution = subprocess.run([sys.executable, str(generator), '--output', str(regenerated)],
                               text=True, capture_output=True)
    (independent / 'stdout.log').write_text(execution.stdout)
    (independent / 'stderr.log').write_text(execution.stderr)
    (independent / 'exit-code.txt').write_text(str(execution.returncode) + '\n')
    execution.check_returncode()
    assert regenerated.read_bytes() == reference.read_bytes()
    # Check the frozen test assertions accompanying the success receipt. These
    # cover real Deep EXR inspect/refusal and source preservation inside CTest.
    native_source = args.source / 'tests/test_deep_flatten.cpp'
    text = native_source.read_text()
    for assertion in ('deep_requires_flattening', 'default Deep refusal changed source file',
                      'generated file was not Deep', 'deep_workspace_limit_exceeded',
                      'deep_sample_limit_exceeded', 'Deep source samples modified'):
        assert assertion in text
    command = [sys.executable, str(Path(__file__).with_name('audit-cryptomatte-validation.py'))]
    for key in ('stage', 'source', 'mainline', 'official', 'previous_stage', 'expected_count'):
        command += ['--' + key.replace('_', '-'), str(getattr(args, key))]
    if args.sdk:
        command += ['--sdk', str(args.sdk)]
    subprocess.run(command, check=True)
    summary = load(result / 'cryptomatte-audit.json')
    summary.update(public_sampled_id_cases=7, deep_decimal_references=117,
                   deep_exact_permutation_checks=1404, internal_deep_kernel_only=True,
                   public_deep_default='deep_requires_flattening',
                   deep_test_sha256=digest(stage / 'bin/thumbgen_deep_flatten_tests'),
                   deep_test_source_sha256=digest(native_source),
                   independent_reference=dict(generator_sha256=digest(generator),
                                              frozen_sha256=digest(reference),
                                              regenerated_sha256=digest(regenerated),
                                              precision=100, seed=94198, byte_equal=True),
                   numerical_scope='Decimal relative tolerances; exact permutations within each native process, not cross-libm bit identity',
                   limits='Internal numeric kernel only; public Deep decoding, budgets, visual QA, non-WSL physical platform, licensing and release remain open')
    destination.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'artifacts'}, indent=2))


if __name__ == '__main__':
    main()
