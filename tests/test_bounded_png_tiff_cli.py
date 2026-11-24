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


def packed_row(cells, y, channels, bits, endian, associated=False):
    samples = []
    limit = (1 << bits) - 1
    for x in cells:
        if channels >= 3:
            samples.extend(value * (limit // 255) for value in pixel(x, y, channels))
        else:
            gray = (x * 1823 + y * 771 + 153) & limit
            alpha = ((x + y) % 5) * limit // 4
            samples.append(gray * alpha // limit if associated else gray)
            if channels == 2:
                samples.append(alpha)
    return bytes(samples) if bits == 8 else struct.pack(f"{endian}{len(samples)}H", *samples)


def gray_profile():
    """Synthetic D50 grayscale display profile with a gamma 1.75 curve."""
    white = struct.pack(">3i", *(round(v * 65536) for v in (0.9642, 1, 0.8249)))
    name = b"Synthetic gray\0"
    tags = [(b"desc", b"desc" + bytes(4) + struct.pack(">I", len(name)) + name + bytes(78)),
            (b"wtpt", b"XYZ " + bytes(4) + white),
            (b"kTRC", b"curv" + bytes(4) + struct.pack(">IH", 1, 448))]
    header = bytearray(128)
    header[8:24] = b"\x02\x10\0\0mntrGRAYXYZ "
    struct.pack_into(">6H", header, 24, 2026, 1, 1, 0, 0, 0)
    header[36:40] = b"acsp"
    header[68:80] = white
    table = bytearray(struct.pack(">I", len(tags)))
    body = bytearray()
    for tag, data in tags:
        table += struct.pack(">4sII", tag, 132 + 12 * len(tags) + len(body), len(data))
        body += data + bytes(-len(data) % 4)
    struct.pack_into(">I", header, 0, len(header) + len(table) + len(body))
    return bytes(header + table + body)


def write_png(path, width, height, channels=3, bits=8, interlaced=False, profile=b"", offset=None,
              header_only=False, gamma=None):
    ihdr = struct.pack(">IIBBBBB", width, height, bits, {1: 0, 2: 4, 3: 2, 4: 6}[channels], 0, 0, interlaced)
    x_block, y_block = max(1, width // 47), max(1, height // 47)
    with path.open("wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr))
        if gamma is not None:
            file.write(chunk(b"gAMA", struct.pack(">I", gamma)))
        if profile:
            file.write(chunk(b"iCCP", b"Synthetic profile\x00\x00" + zlib.compress(profile)))
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
                        samples = packed_row((x // x_block for x in range(x0, width, dx)), band, channels, bits, ">")
                        rows = {band: b"\0" + samples}
                    compressed = compressor.compress(rows[band])
                    if compressed:
                        file.write(chunk(b"IDAT", compressed))
            file.write(chunk(b"IDAT", compressor.flush()))
        file.write(chunk(b"IEND", b""))


def write_tiff(path, width, height, block, header_only=False, *, tiled=True, channels=3, bits=8,
               associated=False, profile=b"", orientation=1, white=False):
    # Classic little-endian Deflate. Tiles include right and bottom padding.
    block_width = block if tiled else width
    x_step, y_step = max(1, width // 47), max(1, height // 47)
    with path.open("w+b") as file:
        file.write(b"II" + struct.pack("<HI", 42, 0))
        offsets, counts = [], []
        for y in range(0, height, block):
            for x in range(0, width, block_width):
                offsets.append(file.tell())
                if header_only:
                    payload = zlib.compress(b"")
                else:
                    bands, rows = {}, []
                    for row in range(block if tiled else min(block, height - y)):
                        band = (y + row) // y_step
                        if band not in bands:
                            cells = (col // x_step for col in range(x, x + block_width))
                            bands[band] = packed_row(cells, band, channels, bits, "<", associated)
                        rows.append(bands[band])
                    payload = zlib.compress(b"".join(rows), 1)
                counts.append(len(payload))
                file.write(payload)

        def values(items, type_code):
            data = items if type_code == 7 else struct.pack("<" + {3: "H", 4: "I"}[type_code] * len(items), *items)
            if len(data) <= 4:
                return data.ljust(4, b"\0")
            offset = file.tell()
            file.write(data)
            return struct.pack("<I", offset)

        tags = [(256, 4, [width]), (257, 4, [height]), (258, 3, [bits] * channels), (259, 3, [8]),
                (262, 3, [2 if channels >= 3 else int(not white)]), (274, 3, [orientation]),
                (277, 3, [channels]), (284, 3, [1])]
        if tiled:
            tags += [(322, 4, [block]), (323, 4, [block]), (324, 4, offsets), (325, 4, counts)]
        else:
            tags += [(273, 4, offsets), (278, 4, [block]), (279, 4, counts)]
        if channels in (2, 4):
            tags.append((338, 3, [1 if associated else 2]))
        if profile:
            tags.append((34675, 7, profile))
        tags.sort()
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
        gray_icc = gray_profile()
        gray_layouts = {
            "gray-gamma": (".png", (12000, 6000), dict(channels=1, bits=8, gamma=45455)),
            "gray-alpha-icc": (".png", (3211, 2511), dict(channels=2, bits=16, profile=gray_icc)),
            "gray-tiled": (".tif", (12001, 1703), dict(channels=1, bits=16, orientation=6)),
            "gray-white": (".tif", (3211, 2511), dict(channels=1, bits=8, tiled=False, white=True)),
            "gray-alpha-associated": (".tif", (3211, 2511),
                                      dict(channels=2, bits=16, tiled=False, associated=True, profile=gray_icc)),
        }
        gray = {}
        for name, (suffix, dims, layout) in gray_layouts.items():
            for case, (w, h) in ((name, dims), (f"{name}-small", (407, 307))):
                paths[case] = root / (case + suffix)
                gray[case] = w, h, layout
                if suffix == ".png":
                    write_png(paths[case], w, h, **layout)
                else:
                    write_tiff(paths[case], w, h, 64, **layout)
        # The TIFF adapter excludes MINISWHITE with alpha.
        paths["gray-white-alpha"] = root / "gray-white-alpha.tif"
        paths["gray-white-alpha-small"] = root / "gray-white-alpha-small.tif"
        write_tiff(paths["gray-white-alpha"], 3211, 2511, 64, tiled=False, channels=2, white=True)
        write_tiff(paths["gray-white-alpha-small"], 407, 307, 64, tiled=False, channels=2, white=True)
        write_png(paths["png12"], 12000, 6000, 4, 16, profile=profile)
        write_png(paths["png16"], 16000, 8000)
        write_tiff(paths["tiled12"], 12000, 6000, 64)
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
        write_tiff(paths["huge-tile"], 64, 64, 131072, header_only=True)

        large = {"png12", "png16", "tiled12", "stripped12"} | gray_layouts.keys()
        positive = large | gray.keys() | {"adam7", "offset", "profile", "gray-white-alpha-small"}
        options = {name: dict(background="transparent") for name in paths}
        for name in gray:
            options[name].update(channels="GRAY", part=0, max_decode_mib=128)
        for name in ("gray-tiled", "gray-tiled-small"):
            options[name]["input_space"] = "linear"
        jobs = [dict(id=name, input=str(path), options=options[name],
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
        for name, (w, h, layout) in gray.items():
            record = results[name]
            selected = record["selection"]["chosen"]
            channels = layout["channels"]
            assert selected["group"] == "GRAY" and selected["kind"] == "grey", record
            assert len(selected["channels"]) == channels + 2, record
            bpp = channels * layout["bits"] // 8 if name in gray_layouts else channels * 4
            assert record["decoded_mib"] == w * h * bpp / 1048576, record
            dw, dh = (h, w) if layout.get("orientation") == 6 else (w, h)
            for (limit_w, limit_h), output in zip(sizes, record["outputs"]):
                scale = min(limit_w / dw, limit_h / dh, 1)
                assert (output["width"], output["height"]) == (int(dw * scale + .5), int(dh * scale + .5)), record
            if name in gray_layouts:
                assert record["execution"]["admission_mib"] < 160, record
            if layout.get("profile"):
                assert record["colour"]["icc"]["model"] == "Gray", record
            if layout.get("gamma"):
                assert record["colour"]["basis"] == "metadata", record
        assert results["gray-white-alpha-small"]["decoded_mib"] == 407 * 307 * 8 / 1048576
        rejected = results["gray-white-alpha"]
        assert rejected["error"]["code"] == "task_memory_exceeds_budget" and not rejected["outputs"], rejected
        for name in paths.keys() - positive - {"gray-white-alpha"}:
            record = results[name]
            assert record["error"]["code"] == "decode_limit_exceeded", record
            assert "admission_mib" not in record["execution"], record
            for width, _ in sizes:
                assert not (root / f"{name}-{width}.png").exists()

        reference_input, reference_dir = root / "reference.json", root / "reference"
        reference_input.write_text(json.dumps(dict(sizes=sizes, cases=[
            dict(id=name, input=str(paths[name]), background="transparent",
                 input_space=options[name].get("input_space", "auto")) for name in sorted(positive)])),
            encoding="utf-8")
        references = read_records([str(args.reference_probe.resolve()), str(reference_input), str(reference_dir),
                                   "product-reference"], env)
        assert len(references) == len(positive), references
        for record in references:
            assert record["ok"], record
            for actual, expected in zip(results[record["id"]]["outputs"], record["outputs"]):
                try:
                    compare(actual["path"], expected["path"])
                except AssertionError as error:
                    raise AssertionError(f'{record["id"]}: {error}') from error


if __name__ == "__main__":
    main()
