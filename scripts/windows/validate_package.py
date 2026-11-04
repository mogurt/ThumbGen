"""Unpack an internal candidate and test it without developer DLL/Python paths."""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import struct
import sys
import time
import zipfile
import zlib

import psutil
from jsonschema import Draft202012Validator

from runtime_closure import digest


def png_range(path):
    data, cursor, compressed = path.read_bytes(), 8, b''
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Re-decoded output is not PNG')
    while cursor < len(data):
        length = struct.unpack('>I', data[cursor:cursor + 4])[0]
        kind, payload = data[cursor + 4:cursor + 8], data[cursor + 8:cursor + 8 + length]
        checksum = struct.unpack('>I', data[cursor + 8 + length:cursor + 12 + length])[0]
        if zlib.crc32(kind + payload) != checksum:
            raise ValueError('PNG chunk CRC differs')
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', payload)
        if kind == b'IDAT':
            compressed += payload
        cursor += length + 12
    if (width, height) != (32, 16) or depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError('Unexpected smoke PNG dimensions/layout')
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(compressed)
    if len(raw) != height * (1 + width * channels):
        raise ValueError('PNG decoded size differs')
    previous, pixels, offset = bytearray(width * channels), [], 0
    for _ in range(height):
        mode = raw[offset]
        row = bytearray(raw[offset + 1:offset + 1 + width * channels])
        if mode > 4:
            raise ValueError('Unsupported PNG filter')
        offset += width * channels + 1
        for index in range(len(row)):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            diagonal = previous[index - channels] if index >= channels else 0
            distances = (abs(up - diagonal), abs(left - diagonal), abs(left + up - 2 * diagonal))
            paeth = (left, up, diagonal)[distances.index(min(distances))]
            row[index] = (row[index] + (0, left, up, (left + up) // 2, paeth)[mode]) & 255
        if channels == 4 and any(row[index] != 255 for index in range(3, len(row), 4)):
            raise ValueError('Known opaque fixture acquired transparency')
        pixels.extend(row[index + channel] for index in range(0, len(row), channels) for channel in range(3))
        previous = row
    if max(pixels) < 32 or min(pixels) == max(pixels):
        raise ValueError('Known gradient decoded as black/constant image')
    return dict(width=width, height=height, min=min(pixels), max=max(pixels))


def validate(archive, fixtures, destination):
    if os.name != 'nt':
        raise RuntimeError('Windows package validation requires a Windows host')
    destination.mkdir(parents=True, exist_ok=False)
    expected = Path(str(archive) + '.sha256').read_text(encoding='ascii').split()[0]
    if digest(archive) != expected:
        raise ValueError('Archive checksum mismatch')
    with zipfile.ZipFile(archive) as package:
        members = package.infolist()
        if len(members) > 4096 or sum(item.file_size for item in members) > 128 * 1024 * 1024:
            raise ValueError('Candidate exceeds validation limits')
        names, roots = set(), set()
        for item in members:
            path = PurePosixPath(item.filename)
            if (path.is_absolute() or '..' in path.parts or '\\' in item.filename or ':' in item.filename
                    or item.filename != '/'.join(path.parts) or len(path.parts) < 2
                    or item.filename.casefold() in names or (item.external_attr >> 16) & 0o170000 not in (0, 0o100000)):
                raise ValueError('Unsafe/duplicate archive member: ' + item.filename)
            names.add(item.filename.casefold())
            roots.add(path.parts[0])
        if roots != {archive.stem}:
            raise ValueError('Candidate root/name mismatch')
        for item in members:
            target = destination.joinpath(*PurePosixPath(item.filename).parts)
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError('Archive target escapes validation directory')
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(item) as source, target.open('xb') as output:
                while block := source.read(1024 * 1024):
                    output.write(block)
    root = destination / archive.stem
    checksums = {}
    for line in (root / 'SHA256SUMS').read_text(encoding='ascii').splitlines():
        checksum, relative = line.split('  ', 1)
        path = PurePosixPath(relative)
        if not re.fullmatch('[0-9a-f]{64}', checksum) or path.is_absolute() or '..' in path.parts or '\\' in relative or ':' in relative or relative in checksums:
            raise ValueError('Invalid payload checksum index')
        checksums[relative] = checksum
        if digest(root / relative) != checksum:
            raise ValueError('Payload checksum mismatch: ' + relative)
    actual = {path.relative_to(root).as_posix() for path in root.rglob('*') if path.is_file()}
    if set(checksums) != actual - {'SHA256SUMS'}:
        raise ValueError('Payload checksum coverage differs')
    manifest = json.loads((root / 'DEPENDENCIES.json').read_text(encoding='utf-8'))
    if manifest['redistribution_ready'] or manifest['kind'] != 'internal-evaluation-candidate':
        raise ValueError('Expected internal candidate with release gates retained')
    runtime_names = {item['name'].lower() for item in manifest['runtime_files']}
    if {p.name.lower() for p in (root / 'bin').iterdir()} != runtime_names:
        raise ValueError('Runtime binary file set differs')
    for item in manifest['runtime_files']:
        if digest(root / 'bin' / item['name']) != item['sha256']:
            raise ValueError('Runtime file differs from manifest')
    temporary = destination / 'temp'
    temporary.mkdir()
    windows = Path(os.environ['SystemRoot']).resolve()
    environment = dict(SystemRoot=str(windows), WINDIR=str(windows), PATH=str(windows / 'System32'),
                       TEMP=str(temporary), TMP=str(temporary))
    exe = root / 'bin/thumbgen.exe'
    schema = Path(__file__).resolve().parents[2] / 'schemas/protocol-v1.schema.json'
    validator = Draft202012Validator(json.loads(schema.read_text(encoding='utf-8')))

    def command(arguments):
        proc = subprocess.run([str(exe), *map(str, arguments)], cwd=root, env=environment,
                              capture_output=True, text=True, encoding='utf-8', timeout=60)
        if proc.returncode:
            raise RuntimeError('Packaged command failed: ' + proc.stdout + proc.stderr)
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
        for row in rows:
            validator.validate(row)
        return rows

    identity = command(['--version', '--json'])[0]
    caps = command(['capabilities', '--json'])[0]
    if identity != manifest['identity'] or caps['cpu']['oiio_simd'] != 'sse2':
        raise ValueError('Packaged identity/SIMD differs')
    sources = [fixtures / ('synthetic.' + extension) for extension in ('png', 'exr', 'jpg', 'webp')]
    before = {path.name: digest(path) for path in sources}
    output = destination / 'outputs'
    output.mkdir()
    request = dict(schema_version=1, defaults=dict(threads=1, preset='balanced', quality=90),
                   execution=dict(workers=2, thread_budget=2, memory_budget_mib=1024), jobs=[])
    for index in range(128):
        source = sources[index % len(sources)]
        request['jobs'].append(dict(id=str(index), input=str(source), outputs=[
            dict(path=str(output / (str(index) + '.' + extension)), format=format, size=[32, 16])
            for extension, format in (('jpg', 'jpeg'), ('png', 'png'), ('webp', 'webp'))]))
    jobs = destination / 'jobs.json'
    jobs.write_text(json.dumps(request, ensure_ascii=False), encoding='utf-8')
    loaded, observations = {}, {}
    with (destination / 'stdout.jsonl').open('wb') as stdout, (destination / 'stderr.txt').open('wb') as stderr:
        proc = psutil.Popen([str(exe), 'batch', '--manifest', str(jobs), '--json'], cwd=root,
                            env=environment, stdout=stdout, stderr=stderr)
        started = time.monotonic()
        try:
            while proc.poll() is None:
                if time.monotonic() - started > 120:
                    raise TimeoutError('Candidate batch exceeded timeout')
                try:
                    processes = [proc, *proc.children(recursive=True)]
                except psutil.NoSuchProcess:
                    break
                for process in processes:
                    try:
                        observation = observations.setdefault(process.pid, dict(executable=process.exe(), modules=set()))
                        for mapping in process.memory_maps(grouped=True):
                            path = Path(mapping.path).resolve()
                            if path.suffix.lower() not in ('.exe', '.dll'):
                                continue
                            if path.name.lower() in runtime_names:
                                if path != (root / 'bin' / path.name).resolve():
                                    raise ValueError('Bundled module loaded from external path: ' + str(path))
                            elif not path.is_relative_to(windows):
                                raise ValueError('Unexpected external module: ' + str(path))
                            loaded[str(path)] = dict(name=path.name, bundled=path.is_relative_to(root))
                            observation['modules'].add(str(path))
                    except psutil.NoSuchProcess:
                        continue
                time.sleep(.01)
            if proc.wait(timeout=10):
                raise RuntimeError('Candidate batch failed; stdout/stderr retained')
        finally:
            if proc.poll() is None:
                for child in proc.children(recursive=True):
                    try:
                        child.kill()
                        child.wait(timeout=10)
                    except psutil.NoSuchProcess:
                        pass
                proc.kill()
                proc.wait(timeout=10)
    records = [json.loads(line) for line in (destination / 'stdout.jsonl').read_text(encoding='utf-8').splitlines() if line.strip()]
    for row in records:
        validator.validate(row)
    results = [row for row in records if row['type'] == 'result']
    outputs = [entry for row in results for entry in row['outputs']]
    if len(results) != 128 or len(outputs) != 384 or any(entry['status'] != 'success' for entry in outputs):
        raise ValueError('Expected 128 tasks and 384 successful previews')
    if {path.name: digest(path) for path in sources} != before:
        raise ValueError('Smoke fixture changed')
    workers_observed = [pid for pid, info in observations.items() if pid != proc.pid
                        and Path(info['executable']).resolve() == exe.resolve()
                        and {Path(path).name.lower() for path in info['modules'] if Path(path).is_relative_to(root)} == runtime_names]
    if {value['name'].lower() for value in loaded.values() if value['bundled']} != runtime_names or len(workers_observed) < 2:
        raise ValueError('Runtime module/worker observation incomplete')
    # Actual re-decode through the packaged process, not metadata-only inspect.
    redecode = []
    for entry in outputs[:12]:
        target = output / ('redecode-' + Path(entry['path']).name + '.png')
        row = command(['convert', entry['path'], '--output', target, '--json'])[0]
        if not row['ok'] or not target.is_file():
            raise ValueError('Packaged output re-decode failed')
        redecode.append(dict(input=entry['path'], sha256=digest(target), pixels=png_range(target)))
    report = dict(archive=str(archive), archive_sha256=digest(archive), unpacked_root=str(root),
                  payload_files=len(actual), identity=identity, tasks=128, outputs=384, redecode=len(redecode),
                  sources_unchanged=True, bundled_images=len(runtime_names), workers_observed=workers_observed,
                  observed_processes={str(pid): dict(executable=info['executable'], modules=sorted(info['modules'])) for pid, info in observations.items()},
                  modules=loaded, developer_library_environment_removed=True,
                  redecode_checks=redecode, test_tools=dict(python=sys.version, psutil=psutil.__version__, jsonschema=importlib.metadata.version('jsonschema')),
                  runtime_python_required=False, minimum_platform_verified=False, redistribution_ready=False)
    (destination / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True, help='New, non-existing validation directory')
    args = parser.parse_args()
    result = validate(args.archive.resolve(), args.fixtures.resolve(), args.destination.resolve())
    print(json.dumps({key: value for key, value in result.items() if key not in ('modules', 'identity', 'observed_processes', 'redecode_checks')}, indent=2))
