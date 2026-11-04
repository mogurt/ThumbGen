"""Unpack and exercise a candidate with no developer library environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--archive', required=True, type=Path)
parser.add_argument('--fixtures', required=True, type=Path)
parser.add_argument('--readelf', default='readelf', help='External audit tool; never part of runtime candidate')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
label = os.environ.get('WSL_DISTRO_NAME', 'linux')
parent = repo / '.deps/linux/package-runs' / label
parent.mkdir(parents=True, exist_ok=True)
result_dir = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
environment = dict(PATH='/usr/bin:/bin', HOME=str(Path.home()), LANG='C', LC_ALL='C')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def command(*values):
    process = subprocess.run(values, env=environment, capture_output=True, text=True, timeout=60)
    if process.returncode:
        raise RuntimeError(str(values) + '\n' + process.stdout + '\n' + process.stderr)
    return process.stdout
archive = args.archive.resolve()
expected = Path(str(archive) + '.sha256').read_text().split()[0]
if digest(archive) != expected:
    raise SystemExit('Archive checksum mismatch')
with tarfile.open(archive) as source:
    for member in source.getmembers():
        path = Path(member.name)
        if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir() or member.issym()):
            raise SystemExit('Unsafe candidate archive member')
        if member.issym() and (Path(member.linkname).is_absolute() or '..' in Path(member.linkname).parts):
            raise SystemExit('Unsafe candidate symlink')
    source.extractall(result_dir)
roots = [path for path in result_dir.iterdir() if path.is_dir()]
if len(roots) != 1:
    raise SystemExit('Expected one candidate root')
root = roots[0]
for row in (root / 'SHA256SUMS').read_text().splitlines():
    checksum, relative = row.split('  ', 1)
    if Path(relative).is_absolute() or '..' in Path(relative).parts:
        raise SystemExit('Unsafe checksum path')
    if digest(root / relative) != checksum:
        raise SystemExit('Payload checksum mismatch: ' + relative)
allowed_root = {'bin', 'lib', 'licenses', 'thumbgen', 'NOTICE', 'README.txt', 'DEPENDENCIES.json', 'ABI_REQUIREMENTS.json', 'SHA256SUMS'}
if {path.name for path in root.iterdir()} != allowed_root:
    raise SystemExit('Unexpected runtime candidate top-level payload')
for path in root.rglob('*'):
    if path.is_file() and path.suffix in ('.py', '.cpp', '.h', '.a', '.whl'):
        raise SystemExit('Development material leaked into candidate: ' + str(path))
exe = root / 'thumbgen'
version = json.loads(command(str(exe), '--version', '--json'))
capabilities = json.loads(command(str(exe), 'capabilities', '--json'))
manifest = json.loads((root / 'DEPENDENCIES.json').read_text())
if version != manifest['identity']:
    raise SystemExit('Candidate identity mismatch')
before = {path.name: digest(path) for path in args.fixtures.iterdir() if path.is_file()}
conversions = []
def png_pixels(path):
    data, cursor, compressed = path.read_bytes(), 8, b''
    while cursor < len(data):
        length = struct.unpack('>I', data[cursor:cursor + 4])[0]
        kind, payload = data[cursor + 4:cursor + 8], data[cursor + 8:cursor + 8 + length]
        cursor += length + 12
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', payload)
        if kind == b'IDAT':
            compressed += payload
    if depth != 8 or colour not in (2, 6) or interlace:
        raise RuntimeError('Unexpected validation output PNG layout')
    channels = 3 if colour == 2 else 4
    raw, previous, pixels, offset = zlib.decompress(compressed), bytearray(width * channels), [], 0
    def paeth(a, b, c):
        p = a + b - c
        distances = (abs(p - a), abs(p - b), abs(p - c))
        return (a, b, c)[distances.index(min(distances))]
    for _ in range(height):
        mode = raw[offset]
        row = bytearray(raw[offset + 1:offset + 1 + width * channels])
        offset += width * channels + 1
        for index in range(len(row)):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            diagonal = previous[index - channels] if index >= channels else 0
            row[index] = (row[index] + (0, left, up, (left + up) // 2, paeth(left, up, diagonal))[mode]) & 255
        pixels.extend(row[index:index + 3] for index in range(0, len(row), channels))
        previous = row
    return width, height, sum(sum(pixel) for pixel in pixels) / (len(pixels) * 3)
for filename in ('synthetic.png', 'synthetic.exr'):
    source = args.fixtures.resolve() / filename
    output = result_dir / (filename + '.preview.png')
    records = [json.loads(line) for line in command(str(exe), 'convert', str(source), '--output', str(output), '--json').splitlines()]
    image = next(record for record in records if record['type'] == 'result')
    if image['status'] != 'success' or image['colour']['config_basis'] != 'builtin':
        raise RuntimeError('Conversion failed or used an external OCIO config')
    width, height, mean = png_pixels(output)
    if (width, height) != (12, 8) or not 10 < mean < 250:
        raise RuntimeError('Synthetic image dimensions or nonblack pixel check failed')
    conversions.append(dict(input=filename, output_sha256=digest(output), mean_rgb=mean, records=records))
after = {path.name: digest(path) for path in args.fixtures.iterdir() if path.is_file()}
if before != after:
    raise RuntimeError('Fixture contents changed')
loader = command('ldd', str(root / 'bin/thumbgen'))
if 'not found' in loader:
    raise RuntimeError('Unresolved candidate dependency')
objects = [root / 'bin/thumbgen']
objects.extend(Path(value).resolve() for value in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader))
bundled_names = {path.name for path in (root / 'lib').iterdir()}
for line in loader.splitlines():
    parts = line.split()
    if parts and parts[0] in bundled_names and (len(parts) < 3 or not Path(parts[2]).resolve().is_relative_to(root)):
        raise RuntimeError('A bundled library was loaded from outside the unpacked candidate')
abi, failures = [], []
glibc = command('getconf', 'GNU_LIBC_VERSION').strip()
for path in sorted(set(objects)):
    if not path.is_file():
        continue
    versions = command(args.readelf, '--version-info', '--wide', str(path))
    needs = versions.split('Version needs section', 1)[-1] if 'Version needs section' in versions else ''
    required = sorted(set(re.findall(r'Name: ((?:GLIBCXX|GLIBC|CXXABI)_[A-Za-z0-9_.]+)', needs)))
    bundled = path.is_relative_to(root)
    for value in required:
        if value.startswith('GLIBC_') and value != 'GLIBC_PRIVATE' and (bundled or glibc == 'glibc 2.17'):
            if not re.fullmatch(r'GLIBC_[0-9.]+', value) or tuple(map(int, value[6:].split('.'))) > (2, 17):
                failures.append(str(path) + ': ' + value)
    abi.append(dict(path=str(path), bundled=bundled, required_versions=required))
report = dict(archive=str(archive), archive_sha256=expected, extracted_root=str(root), glibc=glibc,
              environment=environment, version=version, capabilities=capabilities, conversions=conversions,
              audit_tool=args.readelf,
              fixture_sha256=before, inputs_unchanged=before == after, loader=loader, abi=abi, failures=failures,
              clean_machine_test=False, physical_minimum_platform_test=False)
(result_dir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(dict(report=str(result_dir / 'report.json'), glibc=glibc, conversions=len(conversions),
                      archive_sha256=expected, abi_objects=len(abi), failures=failures)))
raise SystemExit(bool(failures))
