#!/usr/bin/env python3
"""Small independent flat-alpha reference experiment (not release acceptance).

Only EXR fixture I/O uses OpenImageIO/numpy. The oracle uses Decimal arithmetic;
PNG fixture writing and output decoding use the Python standard library.
"""
import argparse
from decimal import Decimal as D, ROUND_FLOOR, getcontext
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

getcontext().prec = 50
ROOT = Path(__file__).resolve().parents[2]
ZERO = D(0)
ONE = D(1)
RGB_TOL = D(1) / 255  # one code value, including codec/OCIO float rounding
ALPHA_TOL = D('0.5001') / 255  # half a code value plus float arithmetic margin


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def decode_srgb(v):
    return v / D('12.92') if v <= D('0.04045') else ((v + D('.055')) / D('1.055')) ** D('2.4')


def encode_srgb(v):
    v = max(ZERO, min(ONE, v))
    return v * D('12.92') if v <= D('0.0031308') else D('1.055') * v ** (ONE / D('2.4')) - D('.055')


def write_png(path, pixels, w, h):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    rows = b''.join(b'\0' + bytes(v for p in pixels[y*w:(y+1)*w] for v in p) for y in range(h))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                     + chunk(b'sRGB', b'\0') + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def read_png(path):
    """Decode raw stored straight 8-bit RGB(A), without a colour or alpha API."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not PNG')
    pos, compressed, header = 8, bytearray(), None
    while pos < len(data):
        n = struct.unpack_from('>I', data, pos)[0]
        tag, payload = data[pos+4:pos+8], data[pos+8:pos+8+n]
        if len(payload) != n or zlib.crc32(tag + payload) & 0xffffffff != struct.unpack_from('>I', data, pos+8+n)[0]:
            raise ValueError('PNG CRC/length mismatch')
        if tag == b'IHDR':
            header = struct.unpack('>IIBBBBB', payload)
        if tag == b'IDAT':
            compressed.extend(payload)
        pos += n + 12
        if tag == b'IEND':
            break
    w, h, depth, kind, comp, filt, interlace = header
    if depth != 8 or kind not in (2, 6) or (comp, filt, interlace) != (0, 0, 0):
        raise ValueError('requires noninterlaced RGB(A)8 PNG')
    bands = 3 if kind == 2 else 4
    raw = zlib.decompress(compressed)
    stride = w * bands
    if len(raw) != h * (stride + 1):
        raise ValueError('PNG decompressed length mismatch')
    previous, pixels = bytearray(stride), []
    for y in range(h):
        offset = y * (stride + 1)
        mode, row = raw[offset], bytearray(raw[offset+1:offset+1+stride])
        if mode not in range(5):
            raise ValueError('invalid PNG filter')
        for i in range(stride):
            a = row[i-bands] if i >= bands else 0
            b, c = previous[i], previous[i-bands] if i >= bands else 0
            p = a + b - c
            distances = (abs(p-a), abs(p-b), abs(p-c))
            predictor = (0, a, b, (a+b)//2, (a, b, c)[distances.index(min(distances))])[mode]
            row[i] = (row[i] + predictor) & 255
        pixels.extend(tuple(row[i:i+bands]) for i in range(0, stride, bands))
        previous = row
    return w, h, bands, pixels


def pattern(w=16, h=12, hidden_variant=False, all_zero=False):
    """Asymmetric curved/stair edge, thin coverage and internal holes."""
    pixels = []
    levels = (0, 1, 8, 32, 64, 127, 192, 254, 255)
    for y in range(h):
        for x in range(w):
            distance = (2*x-15)**2 + (3*y-16)**2
            a = levels[(x+3*y) % len(levels)] if distance < 240 else 0
            if (x+2*y) % 11 == 0 or all_zero:
                a = 0
            rgb = ((19*x+13*y+23) % 256, (7*x+31*y+61) % 256, (47*x+3*y+17) % 256)
            if a == 0:
                rgb = (0, 255, 255) if hidden_variant else (255, 0, 255)
            pixels.append((*rgb, a))
    return pixels


def associated(p):
    a = D(p[3]) / 255
    return tuple(decode_srgb(D(v)/255)*a for v in p[:3]) + (a,)


def write_exr(path, pixels, data, display):
    import OpenImageIO as oiio
    import numpy as np
    x, y, w, h = data
    spec = oiio.ImageSpec(w, h, 4, oiio.FLOAT)
    spec.x, spec.y = x, y
    spec.full_x, spec.full_y, spec.full_width, spec.full_height = display
    spec.channelnames = ('R', 'G', 'B', 'A')
    spec.alpha_channel = 3
    spec.attribute('compression', 'zip')
    spec.attribute('oiio:ColorSpace', 'linear')
    spec.attribute('oiio:UnassociatedAlpha', 0)
    array = np.array([[float(v) for v in p] for p in pixels], dtype=np.float32).reshape(h, w, 4)
    out = oiio.ImageOutput.create(str(path))
    if not out or not out.open(str(path), spec) or not out.write_image(array) or not out.close():
        raise RuntimeError('EXR write failed: ' + oiio.geterror())
    inp = oiio.ImageInput.open(str(path))
    if not inp:
        raise RuntimeError(oiio.geterror())
    s = inp.spec()
    actual_windows = (s.x, s.y, s.width, s.height, s.full_x, s.full_y, s.full_width, s.full_height)
    decoded = inp.read_image(format=oiio.FLOAT)
    closed = inp.close()
    if actual_windows != (*data, *display) or not closed or decoded is None or not np.array_equal(decoded, array):
        raise RuntimeError('EXR fixture changed windows or FLOAT values on disk')
    # Reference starts at exactly the stored FLOAT bits, not a library transform.
    return [tuple(D.from_float(float(v)) for v in p) for p in array.reshape(-1, 4)]


def make_fixtures(root, hdr=False):
    result = []
    for name, variant, zero in [('straight', False, False), ('hidden_variant', True, False), ('zero_hidden', False, True)]:
        raw = pattern(hidden_variant=variant, all_zero=zero)
        path = root / (name + '.png')
        write_png(path, raw, 16, 12)
        if read_png(path) != (16, 12, 4, raw):
            raise RuntimeError('PNG fixture roundtrip failed')
        result.append(dict(name=name, path=path, pixels=[associated(p) for p in raw],
                           data=(0, 0, 16, 12), display=(0, 0, 16, 12), input_space='sRGB', association='straight'))
    layouts = [('associated', (0, 0, 16, 12), (0, 0, 16, 12)),
               ('negative_crop', (-5, -3, 11, 9), (-7, -5, 16, 12)),
               ('outside_crop', (-6, -4, 15, 12), (-4, -3, 16, 12))]
    for name, data, display in layouts:
        path = root / (name + '.exr')
        pixels = write_exr(path, [associated(p) for p in pattern(data[2], data[3])], data, display)
        result.append(dict(name=name, path=path, pixels=pixels, data=data, display=display,
                           input_space='linear', association='associated'))
    if hdr:
        for name, data, display in [('hdr', (0, 0, 16, 12), (0, 0, 16, 12)),
                                    ('hdr_negative_crop', (-5, -3, 11, 9), (-7, -5, 16, 12))]:
            values = (D('-.125'), D('.03125'), D('.25'), D('1'), D('4'), D('16'))
            pixels = [(values[(x+2*y)%6]/2, values[(3*x+y+1)%6]/2, values[(x+y+3)%6]/2, D('.5'))
                      for y in range(data[3]) for x in range(data[2])]
            path = root / (name + '.exr')
            stored = write_exr(path, pixels, data, display)
            result.append(dict(name=name, path=path, pixels=stored, data=data, display=display,
                               input_space='linear', association='associated'))
    return result


def canvas(fixture):
    dx, dy, dw, dh = fixture['data']
    fx, fy, fw, fh = fixture['display']
    black = (ZERO, ZERO, ZERO, ZERO)
    return [fixture['pixels'][(y-dy)*dw+x-dx] if dx <= x < dx+dw and dy <= y < dy+dh else black
            for y in range(fy, fy+fh) for x in range(fx, fx+fw)]


PI = D('3.1415926535897932384626433832795028841971693993751')


def sin_decimal(x):
    """Independent Taylor series, range-reduced; never calls OIIO's kernel."""
    x = x % (2*PI)
    if x > PI:
        x -= 2*PI
    term = total = x
    for n in range(1, 100):
        term *= -x*x / ((2*n)*(2*n+1))
        total += term
        if abs(term) < D('1e-55'):
            return total
    raise ArithmeticError('Decimal sine did not converge')


def lanczos3(x):
    x = abs(x)
    if not x:
        return ONE
    if x >= 3 or x == x.to_integral_value():
        return ZERO
    return 3*sin_decimal(PI*x)*sin_decimal(PI*x/3)/(PI*PI*x*x)


@lru_cache(maxsize=256)
def weights(out_index, scale, size, preset):
    """Pixel-centred separable box/triangle, widened by integer reduction.

    Outside the complete display canvas clamp to its edge; missing DATA inside
    that canvas is already zero. This is not a call into the product or OIIO.
    """
    centre = (D(out_index) + D('.5')) * scale - D('.5')
    if preset not in ('fast', 'balanced', 'quality'):
        raise ValueError('unknown reference filter')
    radius = D(scale) / 2 if preset == 'fast' else D(scale)*(3 if preset == 'quality' else 1)
    pairs = []
    for i in range(int(centre-radius)-1, int(centre+radius)+2):
        d = abs(D(i)-centre)
        weight = (ONE if d < radius else ZERO) if preset == 'fast' else max(ZERO, ONE-d/radius) if preset == 'balanced' else lanczos3(d/scale)
        if weight:
            pairs.append((max(0, min(size-1, i)), weight))
    total = sum(w for _, w in pairs)
    return [(i, w/total) for i, w in pairs]


def reference(fixture, scale, preset, background):
    fw, fh = fixture['display'][2:]
    w, h = fw//scale, fh//scale
    source, result = canvas(fixture), []
    for y in range(h):
        wy = weights(y, scale, fh, preset)
        for x in range(w):
            wx = weights(x, scale, fw, preset)
            p = [sum(source[j*fw+i][c]*a*b for i, a in wx for j, b in wy) for c in range(4)]
            alpha = p[3]
            coverage = max(ZERO, min(ONE, alpha))
            rgb = [v / alpha if alpha > 0 else ZERO for v in p[:3]] if background == 'transparent' else [v + (ONE-coverage if background == 'white' else ZERO) for v in p[:3]]
            result.append(tuple(encode_srgb(v) for v in rgb) + ((alpha,) if background == 'transparent' else ()))
    return w, h, result


def storage_reference(target):
    """RGBA8 zero-alpha-rgb-zero-v1; mathematical filtering stays untouched.

    Decide from the independent expected Alpha, never the product Alpha byte.
    """
    if len(target) == 4:
        alpha = max(ZERO, min(ONE, target[3]))
        if int(alpha*255 + D('.5')) == 0:
            return (ZERO, ZERO, ZERO, alpha)
        return (*target[:3], alpha)
    return target


def compare(path, expected):
    w, h, ref = expected
    aw, ah, bands, pixels = read_png(path)
    if (aw, ah, bands) != (w, h, len(ref[0])):
        raise ValueError('output shape/association mismatch')
    max_rgb, max_alpha, squared, bad, worst = ZERO, ZERO, ZERO, 0, None
    max_visible_rgb, zero_alpha_rgb_failures = ZERO, 0
    for index, (p, target) in enumerate(zip(pixels, ref)):
        target = storage_reference(target)
        for c, (actual, desired) in enumerate(zip(p, target)):
            error = abs(D(actual)/255-desired)
            squared += error*error
            if c < 3:
                max_rgb = max(max_rgb, error)
                if bands == 3 or p[3] > 0:
                    max_visible_rgb = max(max_visible_rgb, error)
                elif error > RGB_TOL:
                    zero_alpha_rgb_failures += 1
            else:
                max_alpha = max(max_alpha, error)
            if worst is None or error > D(worst['error']):
                worst = dict(x=index % w, y=index//w, channel='RGBA'[c], actual_code=actual,
                             actual_alpha_code=p[3] if bands == 4 else 255,
                             expected_normalized=str(desired), error=str(error))
            # Canonical storage is exact, not an extra tolerance exemption.
            bad += (error > (RGB_TOL if c < 3 else ALPHA_TOL) or
                    (c < 3 and bands == 4 and p[3] == 0 and actual != 0))
    return dict(passed=bad == 0, failed_components=bad, max_rgb_error=float(max_rgb),
                max_alpha_error=float(max_alpha), max_visible_rgb_error=float(max_visible_rgb),
                zero_alpha_rgb_failed_components=zero_alpha_rgb_failures,
                rmse=float((squared/(w*h*bands)).sqrt()), worst=worst)


def self_test():
    """Hand-computed anchors and a deliberately corrupted result, no product."""
    assert abs(encode_srgb(D('.5')) - D('.7353569830524494')) < D('1e-15')
    assert abs(decode_srgb(encode_srgb(D('.2'))) - D('.2')) < D('1e-45')
    assert weights(1, 2, 8, 'fast') == [(2, D('.5')), (3, D('.5'))]
    assert weights(1, 2, 8, 'balanced') == [(1, D('.125')), (2, D('.375')), (3, D('.375')), (4, D('.125'))]
    assert sum(w for _, w in weights(0, 2, 8, 'balanced')) == 1
    assert lanczos3(ZERO) == 1 and all(lanczos3(D(i)) == 0 for i in (1, 2, 3, 4))
    assert abs(sin_decimal(PI/2)-1) < D('1e-45')
    assert abs(lanczos3(D('.5'))-D(6)/(PI*PI)) < D('1e-45')
    assert lanczos3(D('1.5')) < 0 and lanczos3(D('2.5')) > 0
    assert abs(sum(w for _, w in weights(0, 4, 16, 'quality'))-1) < D('1e-45')
    assert storage_reference((ONE, ONE, ONE, D('-.1'))) == (ZERO,)*4
    assert storage_reference((ONE, ONE, ONE, D('1.1'))) == (ONE,)*4
    fixture = dict(data=(-2, -1, 1, 1), display=(-3, -2, 2, 2),
                   pixels=[(D('.05'), D('.1'), D('.2'), D('.25'))])
    assert canvas(fixture) == [(ZERO,)*4]*3 + fixture['pixels']
    w, h, ref = reference(fixture, 2, 'fast', 'transparent')
    assert (w, h) == (1, 1) and ref[0][3] == D('.0625')
    assert abs(ref[0][0] - encode_srgb(D('.2'))) < D('1e-45')
    base = ROOT/'.deps/alpha-reference'
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as temp:
        path = Path(temp)/'probe.png'
        write_png(path, [(51, 102, 204, 64)], 1, 1)
        assert read_png(path) == (1, 1, 4, [(51, 102, 204, 64)])
        expected = (1, 1, [(D(51)/255, D(102)/255, D(204)/255, D(64)/255)])
        assert compare(path, expected)['passed']
        write_png(path, [(54, 102, 204, 64)], 1, 1)
        assert not compare(path, expected)['passed']
        write_png(path, [(51, 102, 204, 65)], 1, 1)
        assert not compare(path, expected)['passed']
        below = (D('.8'), D('.4'), D('.2'), D('.4999')/255)
        above = (D('.8'), D('.4'), D('.2'), D('.5')/255)
        assert storage_reference(below) == (ZERO, ZERO, ZERO, below[3])
        assert storage_reference(above) == above
        write_png(path, [(0, 0, 0, 0)], 1, 1)
        assert compare(path, (1, 1, [below]))['passed']
        write_png(path, [(1, 0, 0, 0)], 1, 1)
        assert not compare(path, (1, 1, [below]))['passed']
        write_png(path, [(0, 0, 0, 0)], 1, 1)
        assert not compare(path, (1, 1, [above]))['passed']
    print('Independent oracle self-test passed (transfer, kernels, negative canvas, PNG, failure detection).')


def run_cli(exe, args, env, directory, label):
    command = [str(exe), *map(str, args), '--json']
    completed = subprocess.run(command, env=env, cwd=directory, text=True, capture_output=True, timeout=30)
    (directory / (label+'.stdout.jsonl')).write_text(completed.stdout, encoding='utf-8')
    (directory / (label+'.stderr.txt')).write_text(completed.stderr, encoding='utf-8')
    if completed.returncode:
        raise RuntimeError(f'{label}: CLI exit {completed.returncode}; see saved stdout/stderr')
    rows = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
    if not rows or any(row.get('ok') is False for row in rows):
        raise RuntimeError(f'{label}: no successful JSON result')
    return command, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true', help='check independent reference anchors without CLI or EXR dependencies')
    parser.add_argument('--exe', type=Path, help='CLI to copy and freeze before executing')
    parser.add_argument('--expected-exe-sha256', help='optional externally pinned SHA-256, checked before and after copy')
    parser.add_argument('--runtime-dir', action='append', type=Path, help='explicit DLL/library search directories, repeatable')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='explicit child environment override; values recorded')
    parser.add_argument('--case', action='append', help='run only this exact case name, repeatable (see docs/alpha-reference.md)')
    parser.add_argument('--preset', action='append', choices=('fast', 'balanced', 'quality'), help='reference presets; default fast and balanced')
    parser.add_argument('--hdr-fixtures', action='store_true', help='add associated linear HDR/negative-value EXR fixtures')
    parser.add_argument('--output', type=Path, default=ROOT/'.deps/alpha-reference/run', help='new directory under this worktree .deps only')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.exe or not args.runtime_dir:
        parser.error('--exe and --runtime-dir are required unless --self-test is used')
    output = args.output.resolve()
    if not output.is_relative_to((ROOT/'.deps').resolve()) or not output.is_relative_to(ROOT):
        parser.error('--output must be inside this worktree .deps')
    if output.exists():
        parser.error('--output must not exist; preserve previous evidence with a new name')
    source = args.exe.resolve()
    original_sha = sha(source)
    if args.expected_exe_sha256 and original_sha != args.expected_exe_sha256.lower():
        parser.error('source executable SHA mismatch')
    runtime_dirs = [p.resolve() for p in args.runtime_dir]
    if any(not p.is_dir() for p in runtime_dirs):
        parser.error('runtime directory does not exist')
    overrides = dict(item.split('=', 1) for item in args.env)
    presets = list(dict.fromkeys(args.preset or ('fast', 'balanced')))
    # Do not inherit an accidental project OCIO/plug-in search configuration.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('OCIO', 'OIIO_', 'OPENIMAGEIO_'))}
    env.update(overrides)
    library_key = 'PATH' if os.name == 'nt' else 'LD_LIBRARY_PATH'
    env[library_key] = os.pathsep.join(map(str, runtime_dirs)) + os.pathsep + (str(Path(os.environ['SystemRoot'])/'System32') if os.name == 'nt' else '')
    env['OMP_NUM_THREADS'] = '1'
    output.mkdir(parents=True)
    frozen_dir = output/'frozen'
    frozen_dir.mkdir()
    exe = frozen_dir/source.name
    shutil.copy2(source, exe)
    if sha(exe) != original_sha or sha(source) != original_sha:
        raise RuntimeError('executable changed during freeze')
    shutil.copy2(__file__, output/'reference-tool.py')
    runtime_files = {str(p): sha(p) for d in runtime_dirs for p in sorted(d.iterdir())
                     if p.is_file() and (p.suffix.lower() == '.dll' or '.so' in p.name or p.suffix == '.dylib')}
    report = dict(schema_version=1, kind='synthetic-flat-alpha-reference', passed=False,
                  exe_source=str(source), exe_frozen=str(exe), exe_sha256=original_sha,
                  storage_policy='rgba8-zero-alpha-rgb-zero-v1',
                  reference_sha256=sha(output/'reference-tool.py'), python=sys.executable, python_version=platform.python_version(),
                  worktree_head=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                  runtime_dirs=list(map(str, runtime_dirs)), runtime_sha256=runtime_files, environment_overrides=overrides,
                  effective_library_path=env[library_key], decimal_precision=getcontext().prec,
                  tolerance=dict(rgb=float(RGB_TOL), alpha=float(ALPHA_TOL), units='normalized stored 8-bit PNG'),
                  presets=presets, hdr_fixtures=args.hdr_fixtures,
                  limits='Flat RGB(A), sRGB/linear Rec.709 only; no ICC/CMYK/Deep, no real assets or release QA claim.',
                  cases=[], fixtures=[], failures=[])
    # Snapshot our committed product source; do not read a dirty main worktree.
    report['source_sha256'] = {str(p.relative_to(ROOT)).replace('\\', '/'): sha(p)
                               for folder in ('src', 'include') for p in sorted((ROOT/folder).rglob('*')) if p.is_file()}
    save(output/'report.json', report)
    try:
        _, report['identity'] = run_cli(exe, ['--version'], env, output, 'version')
        _, report['capabilities'] = run_cli(exe, ['capabilities'], env, output, 'capabilities')
        fixtures = make_fixtures(output, args.hdr_fixtures)
        import OpenImageIO as oiio
        import numpy as np
        report['fixture_io'] = dict(oiio=oiio.VERSION_STRING, numpy=np.__version__, oiio_module=str(oiio.__file__),
                                    role='EXR fixture I/O only, never reference filtering/colour/compositing')
        outputs = {}
        for f in fixtures:
            initial = sha(f['path'])
            report['fixtures'].append(dict(name=f['name'], path=str(f['path']), sha256=initial,
                                            data_window=f['data'], display_window=f['display'], association=f['association'], input_space=f['input_space']))
            for preset in presets:
                for scale in (1, 2, 4):
                    for background in ('black', 'white', 'transparent'):
                        label = f"{f['name']}-{preset}-{scale}-{background}"
                        if args.case and label not in args.case:
                            continue
                        target = output/(label+'.png')
                        expected = reference(f, scale, preset, background)
                        command, rows = run_cli(exe, ['convert', f['path'], '--output', target, '--size', f'{expected[0]}x{expected[1]}',
                            '--preset', preset, '--background', background, '--input-space', f['input_space'],
                            '--ocio-config', 'ocio://default', '--threads', 1, '--thread-budget', 1], env, output, label)
                        # Keep every expected pixel: a failure remains a small inspectable repro.
                        save(output/(label+'.reference.json'), dict(width=expected[0], height=expected[1],
                             pixels=[[str(v) for v in p] for p in expected[2]],
                             storage_pixels=[[str(v) for v in storage_reference(p)] for p in expected[2]],
                             storage_policy=report['storage_policy']))
                        metrics = compare(target, expected)
                        entry = dict(name=label, command=command, cli_result=rows, output_sha256=sha(target), **metrics)
                        report['cases'].append(entry)
                        outputs[(f['name'], preset, scale, background)] = read_png(target)
                        if not metrics['passed']:
                            report['failures'].append(label)
            if sha(f['path']) != initial:
                raise RuntimeError('source fixture modified by CLI')
            report['fixtures'][-1]['source_unchanged'] = True
        pairs = [(outputs[('straight', p, s, b)], outputs[('hidden_variant', p, s, b)])
                 for p in presets for s in (1, 2, 4) for b in ('black', 'white', 'transparent')
                 if ('straight', p, s, b) in outputs and ('hidden_variant', p, s, b) in outputs]
        report['hidden_rgb_pairs_compared'] = len(pairs)
        report['hidden_rgb_invariance'] = all(a == b for a, b in pairs) if pairs else None
        if report['hidden_rgb_invariance'] is False:
            report['failures'].append('hidden RGB changed output')
        if args.case and set(args.case) != {c['name'] for c in report['cases']}:
            report['failures'].append('requested case was not executed')
        if sha(exe) != original_sha or any(sha(p) != digest for p, digest in runtime_files.items()):
            raise RuntimeError('frozen executable or shared runtime changed during run')
        report['passed'] = not report['failures']
    except Exception as error:
        report['failures'].append(f'{type(error).__name__}: {error}')
    save(output/'report.json', report)
    print(json.dumps(dict(passed=report['passed'], cases=len(report['cases']), failures=report['failures'],
                         max_rgb_error=max((c['max_rgb_error'] for c in report['cases']), default=None),
                         max_alpha_error=max((c['max_alpha_error'] for c in report['cases']), default=None), report=str(output/'report.json'))))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
