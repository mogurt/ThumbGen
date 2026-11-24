"""Validate bounded PNG/TIFF previews against the full-frame colour path."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

import jsonschema
from PIL import Image, ImageChops, ImageCms, ImageStat


def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def pixel(x, y, channels):
    values = ((x * 7 + y * 3) % 256, (x * 2 + y * 11) % 256, ((x + y) * 13) % 256)
    return values + (43 + 172 * ((x + y) % 2),) if channels == 4 else values


def write_png(path, width, height, channels=3, bits=8, interlaced=False, profile=b"", offset=None,
              header_only=False):
    ihdr = struct.pack(">IIBBBBB", width, height, bits, 6 if channels == 4 else 2, 0, 0, interlaced)
    x_block, y_block = max(1, width // 47), max(1, height // 47)
    with path.open("wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr))
        if profile:
            file.write(chunk(b"iCCP", b"Synthetic RGB\x00\x00" + zlib.compress(profile)))
        if offset:
            file.write(chunk(b"oFFs", struct.pack(">iiB", *offset, 0)))
        if not header_only:
            passes = [(0, 0, 1, 1)]
            if interlaced:
                passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
                          (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
            compressor = zlib.compressobj(1)
            for x0, y0, dx, dy in passes:
                if x0 >= width:
                    continue
                rows = {}
                for y in range(y0, height, dy):
                    band = y // y_block
                    if band not in rows:
                        samples = bytes(value for x in range(x0, width, dx)
                                        for value in pixel(x // x_block, band, channels))
                        rows = {band: b"\0" + (samples if bits == 8 else bytes(v for v in samples for _ in range(2)))}
                    compressed = compressor.compress(rows[band])
                    if compressed:
                        file.write(chunk(b"IDAT", compressed))
            file.write(chunk(b"IDAT", compressor.flush()))
        file.write(chunk(b"IEND", b""))


def write_tiled_tiff(path, width, height, tile, header_only=False):
    # Classic little-endian RGB8 with Deflate; include right/bottom padding.
    with path.open("w+b") as file:
        file.write(b"II" + struct.pack("<HI", 42, 0))
        offsets, counts = [], []
        for y in range(0, height, tile):
            for x in range(0, width, tile):
                offsets.append(file.tell())
                if header_only:
                    payload = zlib.compress(b"")
                else:
                    bands = {}
                    for row in range(tile):
                        band = (y + row) // 131
                        if band not in bands:
                            bands[band] = bytes(value for col in range(tile)
                                                for value in pixel((x + col) // 257, band, 3))
                    payload = zlib.compress(b"".join(bands[(y + row) // 131] for row in range(tile)), 1)
                counts.append(len(payload))
                file.write(payload)

        def values(items, type_code):
            data = struct.pack("<" + ("H" if type_code == 3 else "I") * len(items), *items)
            if len(data) <= 4:
                return data.ljust(4, b"\0")
            offset = file.tell()
            file.write(data)
            return struct.pack("<I", offset)

        tags = [(256, 4, [width]), (257, 4, [height]), (258, 3, [8, 8, 8]), (259, 3, [8]),
                (262, 3, [2]), (277, 3, [3]), (284, 3, [1]), (322, 4, [tile]),
                (323, 4, [tile]), (324, 4, offsets), (325, 4, counts)]
        directory = [struct.pack("<HHI", tag, kind, len(items)) + values(items, kind) for tag, kind, items in tags]
        directory_offset = file.tell()
        file.write(struct.pack("<H", len(tags)) + b"".join(directory) + bytes(4))
        file.seek(4)
        file.write(struct.pack("<I", directory_offset))


def read_records(command, env):
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=240)
    assert result.stdout, (result.returncode, result.stderr)
    return [json.loads(line) for line in result.stdout.splitlines()]


def compare(actual, expected):
    with Image.open(actual) as a, Image.open(expected) as b:
        assert a.size == b.size and a.mode == b.mode, (a.size, b.size, a.mode, b.mode)
        stats = ImageStat.Stat(ImageChops.difference(a, b))
        assert max(hi for _, hi in stats.extrema) <= 5 and max(stats.mean) < 0.45, stats.extrema


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--reference-probe", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--schema-dir", type=Path, required=True)
    args = parser.parse_args()
    env = dict(os.environ)
    env.pop("OCIO", None)
    key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
    env[key] = str(args.runtime_dir.resolve()) + os.pathsep + env.get(key, "")
    validator = jsonschema.Draft202012Validator(json.loads(
        (args.schema_dir / "protocol-v1.schema.json").read_text(encoding="utf-8")))
    sizes = [[720, 490], [173, 151]]
    profile = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
    with tempfile.TemporaryDirectory(prefix="bounded-png-tiff-") as tmp:
        root = Path(tmp)
        paths = {name: root / (name + suffix) for name, suffix in [
            ("png12", ".png"), ("png16", ".png"), ("tiled12", ".tif"), ("stripped12", ".tif"),
            ("adam7", ".png"), ("offset", ".png"), ("profile", ".tif"), ("large-adam7", ".png"),
            ("overflow-adam7", ".png"), ("huge-tile", ".tif")]}
        write_png(paths["png12"], 12000, 6000, 4, 16, profile=profile)
        write_png(paths["png16"], 16000, 8000)
        write_tiled_tiff(paths["tiled12"], 12000, 6000, 64)
        tile = Image.new("RGB", (41, 37))
        tile.putdata([pixel(x, y, 3) for y in range(tile.height) for x in range(tile.width)])
        tile.resize((12000, 6000), Image.Resampling.NEAREST).save(paths["stripped12"], compression="tiff_adobe_deflate")
        write_png(paths["adam7"], 301, 289, 4, 16, interlaced=True, profile=profile)
        write_png(paths["offset"], 401, 389, 4, 16, offset=(17, 11))
        alpha = Image.new("RGBA", (407, 307))
        alpha.putdata([pixel(x // 7, y // 11, 4) for y in range(alpha.height) for x in range(alpha.width)])
        alpha.save(paths["profile"], compression="tiff_lzw", icc_profile=profile)
        write_png(paths["large-adam7"], 12000, 6000, 4, 16, interlaced=True)
        write_png(paths["overflow-adam7"], 0x7FFFFFFF, 0x7FFFFFFF, 4, 16, interlaced=True, header_only=True)
        write_tiled_tiff(paths["huge-tile"], 64, 64, 131072, header_only=True)

        large = {"png12", "png16", "tiled12", "stripped12"}
        positive = large | {"adam7", "offset", "profile"}
        jobs = [dict(id=name, input=str(path), options=dict(background="transparent"),
                     outputs=[dict(path=str(root / f"{name}-{w}.png"), size=[w, h], format="png")
                              for w, h in sizes]) for name, path in paths.items()]
        manifest = root / "jobs.json"
        manifest.write_text(json.dumps(dict(schema_version=1, defaults=dict(threads=1, max_decode_mib=512),
            jobs=jobs, execution=dict(workers=1, thread_budget=1, memory_budget_mib=640))), encoding="utf-8")
        records = read_records([str(args.exe.resolve()), "batch", "--manifest", str(manifest), "--json"], env)
        for record in records:
            validator.validate(record)
        results = {r["id"]: r for r in records if r["type"] == "result"}
        assert results.keys() == paths.keys()
        for name in positive:
            record = results[name]
            assert record["ok"], record
            if name in large:
                assert record["execution"]["admission_mib"] < 512, record
        for name in paths.keys() - positive:
            record = results[name]
            assert record["error"]["code"] == "decode_limit_exceeded", record
            assert "admission_mib" not in record["execution"], record
            for width, _ in sizes:
                assert not (root / f"{name}-{width}.png").exists()

        reference_input, reference_dir = root / "reference.json", root / "reference"
        reference_input.write_text(json.dumps(dict(sizes=sizes, cases=[
            dict(id=name, input=str(paths[name]), background="transparent") for name in sorted(positive)])),
            encoding="utf-8")
        references = read_records([str(args.reference_probe.resolve()), str(reference_input), str(reference_dir),
                                   "product-reference"], env)
        assert len(references) == len(positive), references
        for record in references:
            assert record["ok"], record
            for actual, expected in zip(results[record["id"]]["outputs"], record["outputs"]):
                compare(actual["path"], expected["path"])


if __name__ == "__main__":
    main()
