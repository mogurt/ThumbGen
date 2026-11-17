"""Make the README gallery from public EXRs and real ThumbGen previews.

Download the named inputs and license texts from docs/performance/public-assets.json.
Requires Pillow and Matplotlib for page layout; neither is used by ThumbGen.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from matplotlib import font_manager
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[2]
JOBS = [
    ('crypto-room', 'cornellBox_CryptoWildcard.0001.exr',
     ['--visualize', 'cryptomatte', '--channels', 'uCryptoWildcard'], 'transparent'),
    ('crypto-bunny', 'bunny_CryptoAsset.exr',
     ['--visualize', 'cryptomatte', '--channels', 'uCryptoAsset'], 'transparent'),
    ('depth-colour', 'beachball-0001.exr',
     ['--channels', 'RGB', '--input-space', 'linear'], 'black'),
    ('depth-z', 'beachball-0001.exr',
     ['--visualize', 'depth', '--channels', 'Z', '--data-range', '9,11'], 'black'),
    ('ocio-untonemapped', 'asf-ScanLines-StillLife.exr',
     ['--input-space', 'Linear Rec.709 (sRGB)',
      '--ocio-config', 'ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4',
      '--display', 'sRGB - Display', '--view', 'Un-tone-mapped'], 'black'),
    ('ocio-aces', 'asf-ScanLines-StillLife.exr',
     ['--input-space', 'Linear Rec.709 (sRGB)',
      '--ocio-config', 'ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4',
      '--display', 'sRGB - Display', '--view', 'ACES 1.0 - SDR Video'], 'black'),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pair(output, left, right, captions, footer, depth=False):
    """Place intact previews on equal-size panels, without grading or cropping."""
    canvas = Image.new('RGB', (1440, 534 if depth else 482), 'white')
    draw = ImageDraw.Draw(canvas)
    regular = font_manager.findfont('DejaVu Sans')
    bold = font_manager.findfont(font_manager.FontProperties(family='DejaVu Sans', weight='bold'))
    title = ImageFont.truetype(bold, 23)
    caption = ImageFont.truetype(regular, 18)
    for index, path in enumerate((left, right)):
        x = 20 + index * 710
        draw.text((x, 10), captions[index], font=title, fill='#172b45')
        preview = Image.open(path).convert('RGBA')
        preview.thumbnail((690, 390), Image.Resampling.LANCZOS)
        panel = Image.new('RGB', (690, 390), '#0b1018')
        backdrop = Image.new('RGB', preview.size, '#f0f2f5')
        checker = ImageDraw.Draw(backdrop)
        for y in range(0, preview.height, 18):
            for xx in range(0, preview.width, 18):
                if (xx // 18 + y // 18) % 2:
                    checker.rectangle((xx, y, xx + 17, y + 17), fill='#e0e5eb')
        # Alpha is composited only for display. Original PNGs remain in workdir.
        backdrop.paste(preview, (0, 0), preview)
        panel.paste(backdrop, ((690 - preview.width) // 2, (390 - preview.height) // 2))
        canvas.paste(panel, (x, 50))
    if depth:
        for x in range(380):
            v = round(x * 255 / 379)
            draw.line((750 + x, 454, 750 + x, 466), fill=(v, v, v))
        draw.text((750, 473), 'Z = 9 (near)', font=caption, fill='#62748b')
        draw.text((1000, 473), 'Z = 11 (far)', font=caption, fill='#62748b')
    draw.text((20, canvas.height - 29), footer, font=caption, fill='#62748b')
    canvas.save(output, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--thumbgen', default='thumbgen')
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/assets/features')
    parser.add_argument('--workdir', type=Path, default=ROOT / '.local/feature-gallery')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    args.workdir.mkdir(parents=True, exist_ok=True)
    public = json.loads((ROOT / 'docs/performance/public-assets.json').read_text(encoding='utf-8'))
    sources = {f['filename']: f for f in public['files']}
    names = sorted({job[1] for job in JOBS})
    for name in names:
        if digest(args.inputs / name) != sources[name]['sha256']:
            raise ValueError(f'Public source checksum mismatch: {name}')
    license_names = ['Cryptomatte-BSD-3-Clause.txt', 'OpenEXR-BSD-3-Clause.txt']
    for name in license_names:
        expected = next(f for f in public['license_files'] if f['filename'] == name)
        if digest(args.inputs / name) != expected['sha256']:
            raise ValueError(f'License checksum mismatch: {name}')
    version = json.loads(subprocess.run([args.thumbgen, '--version'], check=True, capture_output=True,
                                       text=True, timeout=30).stdout)['version']
    settings = []
    for name, source, flags, background in JOBS:
        target = args.workdir / (name + '.png')
        options = [*flags, '--size', '800', '--background', background, '--threads', '1',
                   '--preset', 'balanced']
        process = subprocess.run([args.thumbgen, 'convert', str(args.inputs / source),
                                  '--output', str(target), *options, '--existing', 'replace', '--json'],
                                 check=True, capture_output=True, text=True, timeout=60)
        (args.workdir / (name + '.jsonl')).write_text(process.stdout, encoding='utf-8')
        records = [json.loads(line) for line in process.stdout.splitlines() if line]
        result = next(r for r in records if r['type'] == 'result')
        summary = next(r for r in records if r['type'] == 'summary')
        if not result['ok'] or result['status'] != 'success' or summary['success'] != 1:
            raise ValueError(f'Preview failed: {name}')
        if name.startswith('ocio-'):
            assert result['colour']['display'] == 'sRGB - Display'
            assert result['colour']['view'] == flags[-1]
        if name == 'depth-z':
            assert result['visualization']['range'] == [9, 11]
        if name.startswith('crypto-'):
            assert result['visualization']['mode'] == 'cryptomatte'
        settings.append({'name': name, 'source': source, 'options': options,
                         'preview_sha256': digest(target)})
    pair(args.output / 'cryptomatte.png', args.workdir / 'crypto-room.png',
         args.workdir / 'crypto-bunny.png', ('Scene ID overview', 'Fine edges & transparency'),
         'Two Cryptomatte scenes. Stable ID colors; transparency comes from coverage.')
    pair(args.output / 'depth.png', args.workdir / 'depth-colour.png',
         args.workdir / 'depth-z.png', ('Color preview', 'Z-depth preview'),
         'Same EXR, different channels. Fixed range in source units; no per-image normalization.', depth=True)
    pair(args.output / 'ocio.png', args.workdir / 'ocio-untonemapped.png',
         args.workdir / 'ocio-aces.png', ('Un-tone-mapped', 'ACES 1.0 - SDR Video'),
         'Same linear Rec.709 EXR, same sRGB display. Only the OCIO view changes.')
    for name in license_names:
        text = (args.inputs / name).read_text(encoding='utf-8')
        text = '\n'.join(line.rstrip() for line in text.splitlines()).rstrip() + '\n'
        (args.output / name).write_text(text, encoding='utf-8', newline='\n')
    # Publish portable source/option information, never private process logs or paths.
    metadata = {'thumbgen_version': version,
                'sources': [{k: sources[name][k] for k in ('filename', 'url', 'page', 'sha256', 'license')}
                            for name in names],
                'previews': settings,
                'layout': 'Uncropped, proportionally resized previews; checkerboard shows transparency.'}
    (args.output / 'sources.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    for name in names:
        assert digest(args.inputs / name) == sources[name]['sha256']
    print('Rendered 3 feature pairs from 6 successful ThumbGen previews; source checksums unchanged.')


if __name__ == '__main__':
    main()
