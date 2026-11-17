"""Image conversion recipes for the public comparison.

All paths and tool identities come from a local manifest, never this source file.
"""
import json
import math
import os
from pathlib import Path
import sys


def dimensions(width, height, box):
    scale = min(1.0, box[0] / width, box[1] / height)
    return [max(1, math.floor(width * scale + .5)),
            max(1, math.floor(height * scale + .5))]


def prepare(tool, case, boxes, output_dir, tools, threads=1, fmt='png'):
    """Return commands and expected paths; no subprocess or timing here."""
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if fmt not in ('png', 'jpeg'):
        raise ValueError('unsupported output format')
    outputs = [output_dir / (str(i) + ('.png' if fmt == 'png' else '.jpg')) for i in range(len(boxes))]
    if tool == 'thumbgen':
        job = {'id': case['id'], 'input': case['input'],
               'options': {'input_space': 'auto', 'background': 'transparent' if fmt == 'png' else 'black'},
               'outputs': [{'path': str(p), 'size': b, 'format': fmt, 'existing': 'replace'}
                           for p, b in zip(outputs, boxes)]}
        manifest = output_dir / 'thumbgen.json'
        manifest.write_text(json.dumps({'schema_version': 1,
            'defaults': {'threads': threads, 'preset': 'balanced', 'quality': 90, 'ocio_config': 'ocio://default',
                         'max_decode_mib': 2048},
            'execution': {'workers': 1, 'thread_budget': threads, 'memory_budget_mib': 8192},
            'jobs': [job]}), encoding='utf-8')
        return [[tools[tool], 'batch', '--manifest', str(manifest), '--json']], outputs
    if tool == 'pillow':
        manifest = output_dir / 'pillow.json'
        manifest.write_text(json.dumps({'input': case['input'], 'boxes': boxes, 'format': fmt,
                                        'outputs': list(map(str, outputs))}), encoding='utf-8')
        return [[sys.executable, str(Path(__file__).with_name('pillow_worker.py')), str(manifest)]], outputs
    if tool == 'vips':
        commands = []
        for box, out in zip(boxes, outputs):
            commands.append([tools[tool], f'--vips-concurrency={threads}', 'thumbnail', case['input'],
                             str(out) + ('[compression=3,bitdepth=8,strip]' if fmt == 'png' else '[Q=90,strip,background=0]'), str(box[0]),
                             '--height', str(box[1]), '--size', 'down', '--linear',
                             '--output-profile', 'srgb'])
        return commands, outputs
    if tool == 'magick':
        command = [tools[tool], '-limit', 'thread', str(threads), case['input'], '-auto-orient']
        for box, out in zip(boxes, outputs):
            encoding = ['-depth', '8', '-strip', '-define', 'png:compression-level=3'] if fmt == 'png' else ['-background', 'black', '-alpha', 'remove', '-alpha', 'off', '-strip', '-quality', '90']
            linearize = ['-set', 'colorspace', 'RGB'] if case.get('linear_input') else ['-colorspace', 'RGB']
            command += ['(', '+clone', *linearize, '-filter', 'Triangle', '-resize',
                        f'{box[0]}x{box[1]}>', '-colorspace', 'sRGB', *encoding,
                        '-write', str(out), '+delete', ')']
        return [command + ['null:']], outputs
    if tool == 'ffmpeg':
        # Candidate: EXR scene-linear; ordinary images use the native encoded path.
        # ICC and alpha/geometry behaviour are explicitly checked, not assumed equal.
        graph = []
        if len(boxes) > 1:
            graph.append('[0:v]split=' + str(len(boxes)) + ''.join(f'[a{i}]' for i in range(len(boxes))))
        for i, box in enumerate(boxes):
            width, height = case['width'], case['height']
            if case.get('exr_windows') and case['exr_windows'].get('dataWindow'):
                x0, y0, x1, y1 = case['exr_windows']['dataWindow']
                width, height = x1-x0+1, y1-y0+1
            w, h = dimensions(width, height, box)
            if case.get('linear_input'):
                filt = (f'zscale=w={w}:h={h}:filter=bilinear:pin=709:tin=linear:min=gbr:'
                        'rin=full:p=709:t=iec61966-2-1:m=gbr:r=full,format=rgba')
            else:
                filt = f'scale={w}:{h}:flags=bilinear,format=rgba'
            graph.append((f'[a{i}]' if len(boxes) > 1 else '[0:v]') + filt + f'[v{i}]')
        command = [tools[tool], '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
                   '-threads', str(threads), '-i', case['input'], '-filter_complex_threads', str(threads),
                   '-filter_complex', ';'.join(graph)]
        for i, out in enumerate(outputs):
            encoding = ['-c:v', 'png', '-compression_level', '3'] if fmt == 'png' else ['-c:v', 'mjpeg', '-q:v', '2', '-pix_fmt', 'yuvj420p']
            command += ['-map', f'[v{i}]', '-frames:v', '1', '-map_metadata', '-1', *encoding,
                        '-threads:v', str(threads), str(out)]
        return [command], outputs
    raise ValueError('Unknown tool: ' + tool)
