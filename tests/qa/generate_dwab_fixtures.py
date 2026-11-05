"""Author normal DWAB HALF/FLOAT/mixed EXRs; inputs are not decoded oracles.

Optional QA-only dependencies: NumPy and Python OpenImageIO. The normal writer
encodes deterministic finite rational patterns, HDR, signed zero and near-zero
samples. Compare decoded Windows/Linux samples, never pre-encoding arrays.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    if args.output.exists():
        p.error('--output must be new')
    import numpy as np
    import OpenImageIO as oiio
    oiio.attribute('threads', 1)
    out = args.output.resolve()
    out.mkdir(parents=True)
    width, height = 1025, 513  # partial 8x8 edges and three DWAB 256-line blocks
    y, x = np.indices((height, width), dtype=np.int32)
    samples = np.stack([((x*13 + y*7 + c*19) % 257 - 128)/16
                        for c in range(3)], axis=-1).astype(np.float32)
    samples[y % 23 == 0] *= np.float32(16)
    edges = np.array([-0., 0., 2.**-24, -2.**-24, 2.**-14, -2.**-14,
                      2.**-20, -2.**-20], dtype=np.float32)
    tiny = np.array([2.**-149, -2.**-149, 2.**-126, -2.**-126,
                     2.**-24, -2.**-24, 0., -0.], dtype=np.float32)
    stripe = y % 19 == 0
    for c in range(3):
        samples[..., c][stripe] = edges[(x[stripe]+c) % len(edges)]
    recipes = [('half', [oiio.HALF]*3), ('float', [oiio.FLOAT]*3),
               ('mixed', [oiio.HALF, oiio.FLOAT, oiio.HALF])]
    report = dict(generator_sha256=digest(Path(__file__)), command=sys.argv,
                  numpy=np.__version__, oiio=oiio.VERSION_STRING,
                  compression='dwab:45', width=width, height=height,
                  threads=1, files=[], inputs_are_oracles=False)
    for name, formats in recipes:
        source = samples.copy()
        if name != 'half':
            source[..., 1][stripe] = tiny[x[stripe] % len(tiny)]
        if name == 'half':
            source = source.astype(np.float16)
        npy = out/(name+'.input.npy')
        np.save(npy, source, allow_pickle=False)
        primary = oiio.FLOAT if name == 'mixed' else formats[0]
        spec = oiio.ImageSpec(width, height, 3, primary)
        spec.channelnames = ['id_a.R', 'id_a.G', 'id_a.B']
        spec.channelformats = formats
        spec.attribute('compression', 'dwab:45')
        spec.attribute('DateTime', '2026:01:01 00:00:00')
        spec.attribute('Software', 'ThumbGen QA DWAB fixture v1')
        filename = out/(name+'.exr')
        writer = oiio.ImageOutput.create(str(filename))
        if not writer or not writer.supports('channelformats'):
            raise RuntimeError('Normal OpenEXR per-channel writer required')
        if not writer.open(str(filename), spec) or not writer.write_image(source):
            raise RuntimeError(writer.geterror())
        if not writer.close():
            raise RuntimeError(writer.geterror())
        reader = oiio.ImageInput.open(str(filename))
        actual = reader.spec()
        types = [str(t) for t in actual.channelformats] or [str(actual.format)]*3
        compression = actual.get_string_attribute('compression')
        level = actual.get_float_attribute('openexr:dwaCompressionLevel', -1)
        if compression != 'dwab' or level != 45 or types != [str(oiio.TypeDesc(t)) for t in formats]:
            raise RuntimeError(f'Encoded header differs: {compression}, {level}, {types}')
        report['files'].append(dict(name=name, path=str(filename), types=types,
            compression=compression, level=level, sha256=digest(filename),
            input_npy_sha256=digest(npy), metadata_xml=actual.to_xml()))
        reader.close()
    report['library_list'] = oiio.getattribute('library_list', oiio.STRING)
    (out/'fixture-generation.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
