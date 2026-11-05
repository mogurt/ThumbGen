"""Execute an isolated overlay candidate and reread every calibration pixel.

Requires the existing packaging validation requirements and Pillow. No dependency
paths enter the child's environment. Failures retain their original raw logs.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
import sys

from PIL import Image
import psutil
from jsonschema import Draft202012Validator

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts/windows'))
from product_validation import validator_identity, VERSION


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def srgb(value):
    return math.floor(255 * (12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055) + .5)


def validate(exe, color, deep, output):
    output.mkdir(parents=True, exist_ok=False)
    exe, color, deep, output = [p.resolve() for p in (exe, color, deep, output)]
    windows = Path(os.environ['SystemRoot']).resolve()
    env = dict(SystemRoot=str(windows), WINDIR=str(windows), PATH=str(windows / 'System32'),
               TEMP=str(output), TMP=str(output))
    repo = Path(__file__).resolve().parents[2]
    schema = repo / 'schemas/protocol-v1.schema.json'
    validator = Draft202012Validator(json.loads(schema.read_text(encoding='utf-8')))
    identity = validator_identity()
    modules, cases, inputs = {}, [], {Path(entry['path']) for entry in identity.values()}
    runtime = {p.name.casefold(): dict(path=str(p.resolve()), sha256=sha(p)) for p in exe.parent.iterdir()
               if p.suffix.lower() in ('.exe', '.dll')}

    def record(path):
        return dict(path=str(path.resolve()), sha256=sha(path))
    color_report = color / 'report.json'
    inputs.add(color_report)
    reference = json.loads(color_report.read_text(encoding='utf-8'))

    def run(name, arguments):
        command = [str(exe), *map(str, arguments), '--json']
        with (output / (name + '.stdout')).open('wb') as stdout, (output / (name + '.stderr')).open('wb') as stderr:
            proc = psutil.Popen(command, cwd=output, env=env, stdout=stdout, stderr=stderr)
            start = time.monotonic()
            try:
                while proc.poll() is None:
                    if time.monotonic() - start > 60:
                        raise TimeoutError(name)
                    try:
                        processes = [proc, *proc.children(recursive=True)]
                    except psutil.NoSuchProcess:
                        break
                    for process in processes:
                        try:
                            for mapping in process.memory_maps(grouped=True):
                                path = Path(mapping.path).resolve()
                                if path.suffix.lower() not in ('.dll', '.exe'):
                                    continue
                                if not (path.is_relative_to(exe.parent) or path.is_relative_to(windows / 'System32') or
                                        path.is_relative_to(windows / 'WinSxS')):
                                    raise ValueError('Unexpected loaded module: ' + str(path))
                                modules[str(path)] = sha(path)
                        except psutil.NoSuchProcess:
                            pass
                    time.sleep(.005)
                code = proc.wait(timeout=5)
            finally:
                if proc.poll() is None:
                    for child in proc.children(recursive=True):
                        child.kill()
                    proc.kill()
                    proc.wait()
        rows = [json.loads(line) for line in (output / (name + '.stdout')).read_bytes().splitlines() if line.strip()]
        for row in rows:
            validator.validate(row)
        cases.append(dict(name=name, command=command, exit=code,
                          stdout=record(output / (name + '.stdout')), stderr=record(output / (name + '.stderr'))))
        (output / 'commands.json').write_text(json.dumps(cases, indent=2), encoding='utf-8')
        if code:
            raise RuntimeError('Packaged command failed: ' + name + '; raw output retained')
        return rows

    caps = run('capabilities', ['capabilities'])[0]
    has_zstd = 'zstd' in caps['deep_compressions']
    for item in reference['cases']:
        source = color / 'fixtures' / item['name']
        key = 'fixtures\\' + item['name']
        if sha(source) != reference['files'][key]:
            raise ValueError('Reviewed calibration fixture differs')
        inputs.add(source)
        target = output / (item['name'] + '.png')
        run(item['name'], ['convert', source, '--output', target, '--size', '8', '--preset', 'fast', '--ocio-config', 'ocio://default'])
        # Recompute author formula; compare the frozen reference as a second check.
        expected = tuple(srgb(v) for v in item['linear_reference'])
        if list(expected) != item['expected']:
            raise ValueError('Author reference formula differs')
        with Image.open(target) as image:
            image.load()
            if image.size != (8, 8):
                raise ValueError('Calibration output dimensions differ')
            delta = max(abs(a - b) for pixel in image.convert('RGB').getdata() for a, b in zip(pixel, expected))
        if delta > 2:
            raise ValueError('ICC calibration pixels differ: ' + item['name'])
        cases[-1].update(max_lsb=delta, pixels=64, expected=expected, output=record(target))
    source = output / 'ordinary-rgb.png'
    Image.new('RGB', (8, 8), (64, 128, 192)).save(source)
    target = output / 'ordinary-rgb-output.png'
    run('ordinary-rgb', ['convert', source, '--output', target, '--size', '8', '--preset', 'fast', '--ocio-config', 'ocio://default'])
    with Image.open(target) as image:
        image.load()
        delta = max(abs(a - b) for pixel in image.convert('RGB').getdata() for a, b in zip(pixel, (64, 128, 192)))
    if delta > 1:
        raise ValueError('Ordinary RGB pixels differ')
    cases[-1].update(max_lsb=delta, pixels=64, output=record(target))
    for layout in (('scan', 'tile') if has_zstd else ()):
        source, reviewed = deep / (layout + '.exr'), deep / (layout + '.png')
        inputs.update((source, reviewed))
        target = output / ('deep-' + layout + '.png')
        run('deep-' + layout, ['convert', source, '--output', target, '--deep-mode', 'flatten', '--threads', '1',
            '--input-space', 'linear', '--ocio-config', 'ocio://default', '--size', '129x9', '--preset', 'fast', '--background', 'black'])
        with Image.open(target) as image, Image.open(reviewed) as prior:
            image.load()
            prior.load()
            if image.size != (129, 9) or list(image.getdata()) != list(prior.getdata()):
                raise ValueError('Deep reviewed output differs')
            delta = 0
            for y in range(9):
                expected = ((0, srgb(.75), srgb(.5)) if y % 4 == 1 else
                            (srgb(.5), srgb(.8125), srgb(.5)) if y % 4 == 2 else (0, 0, 0))
                for x in range(129):
                    delta = max(delta, *(abs(a - b) for a, b in zip(image.getpixel((x, y)), expected)))
            if delta > 1:
                raise ValueError('Independent Deep formula differs')
        cases[-1].update(max_lsb=delta, pixels=1161, output=record(target))
    required = set(runtime)
    observed = {Path(path).name.lower() for path in modules if Path(path).is_relative_to(exe.parent)}
    if not required <= observed:
        raise ValueError('Missing loaded-module observations: ' + str(required - observed))
    (output / 'commands.json').write_text(json.dumps(cases, indent=2), encoding='utf-8')
    (output / 'modules.json').write_text(json.dumps(modules, indent=2), encoding='utf-8')
    inputs.update(p for p in output.iterdir() if p.is_file())
    inputs.update(p for p in exe.parent.iterdir() if p.suffix.lower() in ('.dll', '.exe'))
    report = dict(schema_version=VERSION, kind='independent-windows-product-validation', passed=True,
        exe_sha256=sha(exe), tested_exe=str(exe), tested_runtime=runtime, validator=identity,
        loaded_modules=modules, module_index=record(output / 'modules.json'),
        command_index=record(output / 'commands.json'), cases=cases,
        scope='Synthetic calibration and bounded Deep formula; no production ICC or minimum-machine acceptance',
        artifacts=[dict(path=str(p), sha256=sha(p)) for p in sorted(inputs)],
        environment=env, module_observations=len(modules))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('exe', 'color-evidence', 'deep-evidence', 'output'):
        parser.add_argument('--' + flag, type=Path, required=True)
    args = parser.parse_args()
    result = validate(args.exe, args.color_evidence, args.deep_evidence, args.output)
    print(json.dumps(dict(passed=result['passed'], cases=len(result['cases']), modules=result['module_observations'])))
