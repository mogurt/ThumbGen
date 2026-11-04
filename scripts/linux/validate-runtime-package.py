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
parser.add_argument('--output', type=Path, help='Fresh-run report parent; candidate is never modified')
parser.add_argument('--require-four-formats', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
label = os.environ.get('WSL_DISTRO_NAME', 'linux')
parent = (args.output or repo / '.deps/linux/package-runs') / label
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
listed = set()
for row in (root / 'SHA256SUMS').read_text().splitlines():
    checksum, relative = row.split('  ', 1)
    if Path(relative).is_absolute() or '..' in Path(relative).parts:
        raise SystemExit('Unsafe checksum path')
    if digest(root / relative) != checksum:
        raise SystemExit('Payload checksum mismatch: ' + relative)
    if relative in listed:
        raise SystemExit('Duplicate checksum path')
    listed.add(relative)
actual_files = {str(path.relative_to(root)) for path in root.rglob('*') if path.is_file() and path != root / 'SHA256SUMS'}
if actual_files != listed:
    raise SystemExit('Checksum coverage differs from complete file payload')
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
if manifest['redistribution_ready'] is not False:
    raise SystemExit('Internal candidate must retain unresolved redistribution status')
if manifest.get('packaged_executable_sha256') and digest(root / 'bin/thumbgen') != manifest['packaged_executable_sha256']:
    raise SystemExit('Packaged executable SHA differs from provenance')
if manifest.get('payload_symlinks', {}) != {str(path.relative_to(root)): os.readlink(path) for path in root.rglob('*') if path.is_symlink()}:
    raise SystemExit('Candidate symlink inventory mismatch')
binding = manifest.get('archive_binding')
if version['build']['revision'] == 'unknown':
    source_map = manifest['frozen_source_snapshot']
    if (not binding or binding['commit'] != manifest['frozen_product_commit'] or binding['input_files'] != len(source_map) or
            binding['input_map_sha256'] != hashlib.sha256(json.dumps(source_map, sort_keys=True, separators=(',', ':')).encode()).hexdigest()):
        raise SystemExit('Unknown embedded revision lacks a valid full archive input binding')
for component in manifest['dependency_components']:
    for notice in component['notices']:
        if digest(root / notice['file']) != notice['sha256']:
            raise SystemExit('Component notice checksum mismatch')
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
    mean = sum(sum(pixel) for pixel in pixels) / (len(pixels) * 3)
    left = sum(pixels[y * width][0] for y in range(height)) / height
    right = sum(pixels[y * width + width - 1][0] for y in range(height)) / height
    top = sum(pixels[x][1] for x in range(width)) / width
    bottom = sum(pixels[(height - 1) * width + x][1] for x in range(width)) / width
    stats = dict(mean_rgb=mean, min_rgb=[min(pixel[channel] for pixel in pixels) for channel in range(3)],
                 max_rgb=[max(pixel[channel] for pixel in pixels) for channel in range(3)],
                 distinct_rgb=len({tuple(pixel) for pixel in pixels}),
                 red_left_to_right_delta=right - left, green_top_to_bottom_delta=bottom - top,
                 decoded_rgb_sha256=hashlib.sha256(b''.join(pixels)).hexdigest())
    return width, height, stats

def gradient_check(path):
    width, height, stats = png_pixels(path)
    if ((width, height) != expected_dimensions or not 10 < stats['mean_rgb'] < 250 or
            stats['distinct_rgb'] < 16 or stats['red_left_to_right_delta'] <= 40 or
            stats['green_top_to_bottom_delta'] <= 40):
        raise RuntimeError('Decoded PNG does not retain the expected dimensions/nonconstant RGB gradient: ' + str(path))
    return dict(width=width, height=height, **stats, gradient_valid=True)
expected_dimensions = struct.unpack('>II', (args.fixtures / 'synthetic.png').read_bytes()[16:24])
filenames = ['synthetic.png', 'synthetic.exr']
for optional in ('synthetic.jpg', 'synthetic.webp'):
    if (args.fixtures / optional).is_file():
        filenames.append(optional)
if args.require_four_formats and len(filenames) != 4:
    raise RuntimeError('Four-format fixtures required')
for filename in filenames:
    source = args.fixtures.resolve() / filename
    for kind, suffix in (('png','png'), ('jpeg','jpg'), ('webp','webp')):
        output = result_dir / (filename + '.preview.' + suffix)
        records = [json.loads(line) for line in command(str(exe), 'convert', str(source), '--output', str(output),
                                                      '--format', kind, '--threads', '1', '--json').splitlines()]
        image = next(record for record in records if record['type'] == 'result')
        if image['status'] != 'success' or image['colour']['config_basis'] != 'builtin':
            raise RuntimeError('Conversion failed or used an external OCIO config')
        inspection = json.loads(command(str(exe), 'inspect', str(output), '--json'))
        part = inspection['parts'][0]
        if not inspection['ok'] or (part['width'], part['height']) != expected_dimensions:
            raise RuntimeError('Encoded output metadata inspection or dimensions failed')
        mean = None
        if kind == 'png':
            mean = gradient_check(output)['mean_rgb']
        encoded_sha = digest(output)
        decoded_png = result_dir / (filename + '.preview.' + suffix + '.redecoded.png')
        decoded_records = [json.loads(line) for line in command(str(exe), 'convert', str(output), '--output',
            str(decoded_png), '--format', 'png', '--threads', '1', '--json').splitlines()]
        decoded_result = next(record for record in decoded_records if record['type'] == 'result')
        if decoded_result['status'] != 'success' or digest(output) != encoded_sha:
            raise RuntimeError('Packaged convert failed actual output re-decode or changed its input')
        decoded_pixels = gradient_check(decoded_png)
        conversions.append(dict(input=filename, format=kind, output_sha256=digest(output), mean_rgb=mean,
                                inspection=inspection, inspection_scope='metadata_only', records=records,
                                redecode=dict(method='packaged public convert to PNG, followed by independent png_pixels',
                                    input_sha256=encoded_sha, input_unchanged=True, png=str(decoded_png),
                                    png_sha256=digest(decoded_png), pixels=decoded_pixels, records=decoded_records)))
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
report['complete_payload_checksums_verified'] = True
report['notice_files_verified'] = sum(len(component['notices']) for component in manifest['dependency_components'])
report['archive_binding'] = binding
report['validation_scope'] = dict(metadata_inspections=len(conversions), inspection_scope='metadata_only',
    actual_redecode_conversions=len(conversions), independent_redecoded_png_pixel_checks=len(conversions),
    expected_dimensions=list(expected_dimensions),
    gradient_criteria=dict(distinct_rgb_minimum=16, red_left_to_right_delta_minimum_exclusive=40,
                          green_top_to_bottom_delta_minimum_exclusive=40, mean_rgb_exclusive_range=[10,250]))
(result_dir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(dict(report=str(result_dir / 'report.json'), glibc=glibc, conversions=len(conversions),
                      actual_redecodes=len(conversions), independent_pixel_checks=len(conversions),
                      archive_sha256=expected, abi_objects=len(abi), failures=failures)))
raise SystemExit(bool(failures))
