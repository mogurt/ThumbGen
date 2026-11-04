"""Repeat the single frozen production reproduction through the public CLI."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys

from jsonschema import Draft202012Validator


def verify_reference_input(corpus_path, reference_path):
    """Reject source drift against the frozen Windows baseline before any copying or CLI calls."""
    digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    corpus = json.loads(corpus_path.read_text())
    reference = json.loads(reference_path.read_text())
    if len(corpus) != 1 or not isinstance(reference.get('input_sha256'), dict):
        raise ValueError('Expected one corpus source and explicit frozen input_sha256 mapping')
    identity = corpus[0]['id']
    if set(reference['input_sha256']) != {identity}:
        raise ValueError('Frozen Windows input IDs differ from the unique corpus ID')
    expected = reference['input_sha256'][identity]
    if not isinstance(expected, str) or len(expected) != 64 or any(char not in '0123456789abcdef' for char in expected):
        raise ValueError('Invalid frozen Windows SHA256')
    original = PureWindowsPath(corpus[0]['path'])
    original = (Path('/mnt') / original.drive[0].lower() / Path(*original.parts[1:])
                if original.drive else Path(corpus[0]['path']))
    actual = digest(original)
    if actual != expected:
        raise ValueError(f'Input SHA256 differs from frozen Windows reference: {identity}: expected {expected}, actual {actual}')
    return dict(id=identity, original_path=str(original), expected_sha256=expected, actual_sha256=actual,
                reference_input_hashes_sha256=digest(reference_path), corpus_sha256=digest(corpus_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--reference-report', type=Path)
    parser.add_argument('--reference-input-hashes', type=Path, required=True,
                        help='Frozen Windows report or explicit file containing the unique ID input_sha256 mapping')
    parser.add_argument('--check-input-only', action='store_true', help='Verify the binding without copying, importing benchmarks or converting')
    parser.add_argument('--image-validator', type=Path)
    parser.add_argument('--directory', type=Path)
    args = parser.parse_args()
    input_binding = verify_reference_input(args.corpus, args.reference_input_hashes)
    if args.check_input_only:
        print(json.dumps(dict(input_binding=input_binding, conversion_started=False, copied=False)))
        return
    for name in ('source', 'stage', 'reference_report', 'image_validator', 'directory'):
        if getattr(args, name) is None:
            parser.error('--' + name.replace('_', '-') + ' is required for conversion')
    sys.path.insert(0, str(args.source / 'benchmarks'))
    from cli_timing import native_unit
    digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    module = importlib.util.spec_from_file_location('preview_encoding', Path(__file__).with_name('verify-preview-encoding.py'))
    encoding = importlib.util.module_from_spec(module)
    module.loader.exec_module(encoding)
    schema = Draft202012Validator(json.loads((args.source / 'schemas/protocol-v1.schema.json').read_text()))
    corpus = json.loads(args.corpus.read_text())
    if len(corpus) != 1:
        raise ValueError('Expected the unique one-source production reproduction')
    args.directory.mkdir(exist_ok=False)
    inputs = args.directory / 'inputs'
    inputs.mkdir()
    original = Path(input_binding['original_path'])
    source_sha = digest(original)
    if source_sha != input_binding['expected_sha256']:
        raise ValueError('Source changed after frozen reference verification')
    copied = inputs / original.name
    shutil.copyfile(original, copied)
    assert digest(copied) == source_sha and digest(original) == source_sha
    copied.chmod(0o444)
    inputs.chmod(0o555)
    environment = dict(os.environ, LD_LIBRARY_PATH=str(args.stage / 'lib'), OCIO='ocio://default')
    exe = args.stage / 'bin/thumbgen'
    run_json = lambda command: json.loads(subprocess.check_output(list(map(str, command)), env=environment, text=True))
    capabilities = run_json([exe, 'capabilities', '--json'])
    assert capabilities['quantization'] == 'unorm8-half-up-v1'
    report = dict(product_sha256=digest(exe), product_identity=run_json([exe, '--version', '--json']), capabilities=capabilities,
                  helper_sha256=digest(args.image_validator), source_sha256=source_sha, original_path=str(original), copy_path=str(copied),
                  corpus_sha256=digest(args.corpus), reference_report_sha256=digest(args.reference_report),
                  input_binding=input_binding, runs=[])

    def validate_image(path):
        result = subprocess.run([str(args.image_validator), str(path)], env=environment, capture_output=True, text=True)
        image = json.loads(result.stdout)
        assert result.returncode == 0 and image['readable'] and image['all_black'] is False
        return image

    baseline = None
    for threads in (1, 2, 4, 8):
        for repeat in (1, 2, 3):
            directory = args.directory / f'threads-{threads}-repeat-{repeat}'
            unit = native_unit(exe, environment, [dict(id=corpus[0]['id'], path=str(copied))], directory,
                               threads, 300, formats=('jpeg', 'png', 'webp'), image_validator=validate_image)
            assert len(unit['records']) == 2
            for record in unit['records']:
                schema.validate(record)
            outputs = [Path(output['path']) for record in unit['records'] if record['type'] == 'result' for output in record['outputs']]
            encoded = [encoding.inspect_encoding(path) for path in outputs]
            hashes = [item['sha256'] for item in encoded]
            if baseline is None:
                baseline = hashes
            report['runs'].append(dict(threads=threads, repeat=repeat, unit=unit, encodings=encoded,
                                       matches_first_thread1=hashes == baseline))
            report['source_sha256_unchanged'] = digest(original) == source_sha and digest(copied) == source_sha
            (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            assert unit['valid'] and len(outputs) == 6 and hashes == baseline and report['source_sha256_unchanged']
    report.update(tasks=12, previews=72, schema_records=24, all_sha_equal=True, actual_encoding_checks=72)
    (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(report=str(args.directory / 'report.json'), tasks=12, previews=72, schema_records=24, all_sha_equal=True)))


if __name__ == '__main__':
    main()
