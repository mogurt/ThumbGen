"""Exercise row-backed and frame-backed formats through batch admission."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

import jsonschema
from PIL import Image


COLORS = ((210, 31, 17), (35, 180, 41), (26, 47, 196), (200, 163, 33))


def rows(width, bytes_per_pixel, order, encode, stride=None):
    stride = stride or width * bytes_per_pixel
    for a, b in order:
        left = width // 2
        yield (encode(COLORS[a]) * left + encode(COLORS[b]) * (width - left)).ljust(stride, b"\0")


def write_bmp(path, width, height, top=False, rgba=False, compression=0):
    n = 4 if rgba else 3
    stride = (width * n + 3) & ~3
    with path.open("wb") as file:
        file.write(struct.pack("<2sIII", b"BM", stride * height + 54, 0, 54))
        file.write(struct.pack("<IiiHHIIiiII", 40, width, -height if top else height, 1,
                               n * 8 if not compression else 8, compression, stride * height, 0, 0, 0, 0))
        if not compression:
            order = [(0, 1), (2, 3)] if top else [(2, 3), (0, 1)]
            for row, length in zip(rows(width, n, order, lambda rgb: bytes(rgb[::-1]) +
                                        (b"\xff" if rgba else b""), stride),
                                   (height // 2, height - height // 2)):
                for _ in range(length):
                    file.write(row)


def write_tga(path, width, height, top=False, right=False, rle=False):
    with path.open("wb") as file:
        flags = (32 if top else 0) + (16 if right else 0)
        file.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 10 if rle else 2, 0, 0, 0,
                               0, 0, width, height, 24, flags))
        if not rle:
            order = [(0, 1), (2, 3)] if top else [(2, 3), (0, 1)]
            if right:
                order = [xy[::-1] for xy in order]
            for row, length in zip(rows(width, 3, order, lambda rgb: bytes(rgb[::-1])),
                                   (height // 2, height - height // 2)):
                for _ in range(length):
                    file.write(row)


def write_dpx(path, width, height, mirrored=False, little=True):
    order = "<" if little else ">"
    buf = bytearray(2048)
    buf[:4] = b"XPDS" if little else b"SDPX"
    struct.pack_into(order + "I8sIIIII", buf, 4, len(buf), b"V2.0", len(buf) + 4 * width * height,
                     0, 1664, 384, 0)
    struct.pack_into(order + "HHII", buf, 768, int(mirrored), 1, width, height)
    struct.pack_into(order + "BBBBHHIII", buf, 800, 50, 6, 6, 10, 1, 0, len(buf), 0, 0)
    struct.pack_into(order + "II", buf, 1920, 0xffffffff, 0xffffffff)

    def pixel(rgb):
        r, g, b = (value * 4 for value in rgb)
        return struct.pack(order + "I", (r << 22) | (g << 12) | (b << 2))

    quadrants = [(1, 0), (3, 2)] if mirrored else [(0, 1), (2, 3)]
    with path.open("wb") as file:
        file.write(buf)
        for row, length in zip(rows(width, 4, quadrants, pixel), (height // 2, height - height // 2)):
            for _ in range(length):
                file.write(row)


def write_hdr(path, width, height, mirrored=False, rle=True):
    def repeat(count, value):
        return bytes([255, value]) * (count // 127) + (bytes([128 + count % 127, value]) if count % 127 else b"")

    with path.open("wb") as file:
        direction = "-X" if mirrored else "+X"
        file.write(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=1.0\n\n-Y {height} {direction} {width}\n".encode())
        quadrants = [(1, 0), (3, 2)] if mirrored else [(0, 1), (2, 3)]
        for (left, right), lines in zip(quadrants, (height // 2, height - height // 2)):
            a, b = COLORS[left] + (128,), COLORS[right] + (128,)
            if rle:
                row = b"\x02\x02" + struct.pack(">H", width)
                row += b"".join(repeat(width // 2, x) + repeat(width - width // 2, y) for x, y in zip(a, b))
            else:
                row = bytes(a) * (width // 2) + bytes(b) * (width - width // 2)
            for _ in range(lines):
                file.write(row)


def write_webp(path, width, height, orientation=1, lossless=False):
    image = Image.new("RGB", (width, height))
    for i, rgb in enumerate(COLORS):
        x, y = i % 2, i // 2
        image.paste(rgb, (x * width // 2, y * height // 2,
                          width // 2 if x == 0 else width, height // 2 if y == 0 else height))
    options = {}
    if orientation != 1:
        exif = Image.Exif()
        exif[274] = orientation
        options["exif"] = exif.tobytes()
    image.save(path, quality=95, method=0, lossless=lossless, **options)


def run(args, root, sources, memory, decode, prefix):
    jobs = [
        dict(id=name, input=str(path), outputs=[
            dict(path=str(root / f"{prefix}-{name}-{width}.png"), size=[width, height], format="png")
            for width, height in ((640, 480), (192, 128))])
        for name, path in sources.items()
    ]
    manifest = dict(schema_version=1, defaults=dict(threads=1, max_decode_mib=decode), jobs=jobs,
                    execution=dict(workers=1, thread_budget=1, memory_budget_mib=memory))
    path = root / "jobs.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    env = dict(os.environ)
    env.pop("OCIO", None)
    key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
    env[key] = str(args.runtime_dir.resolve()) + os.pathsep + env.get(key, "")
    completed = subprocess.run([str(args.exe.resolve()), "batch", "--manifest", str(path), "--json"],
                               env=env, capture_output=True, text=True, timeout=180)
    validator = jsonschema.Draft202012Validator(
        json.loads((args.schema_dir / "protocol-v1.schema.json").read_text(encoding="utf-8")))
    results = {}
    for line in completed.stdout.splitlines():
        row = json.loads(line)
        validator.validate(row)
        if row["type"] == "result":
            results[row["id"]] = row
    assert results.keys() == sources.keys(), (completed.stdout, completed.stderr)
    return results


def corners(preview):
    return [preview.getpixel((preview.width * x // 4, preview.height * y // 4))[:3]
            for y in (1, 3) for x in (1, 3)]


def check(record, sizes, colors, tolerance):
    assert record["ok"], record
    for output, size in zip(record["outputs"], sizes):
        with Image.open(output["path"]).convert("RGB") as preview:
            assert preview.size == size, output
            actual = corners(preview)
        assert max(abs(a - b) for val, ref in zip(actual, colors) for a, b in zip(val, ref)) <= tolerance, actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--schema-dir", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="bounded-formats-") as tmp:
        root = Path(tmp)
        writers = dict(hdr=write_hdr, dpx=write_dpx, bmp=write_bmp, webp=write_webp, tga=write_tga)
        large = {name: root / f"large.{name}" for name in writers}
        for name, path in large.items():
            writers[name](path, 12000, 6000)
        results = run(args, root, large, 768, 512, "bounded")
        # HDR stores linear Rec.709, which is encoded into display sRGB.
        hdr_colors = tuple(tuple(round(255 * (1.055 * (value / 256) ** (1 / 2.4) - 0.055))
                                 for value in rgb) for rgb in COLORS)
        for name, record in results.items():
            check(record, [(640, 320), (192, 96)], hdr_colors if name == "hdr" else COLORS, 5)
            admission = record["execution"]["admission_mib"]
            assert admission < (430 if name in ("tga", "webp") else 150), record
            if name in ("tga", "webp"):
                assert admission > 206, record  # The scheduler must account for the RGB frame.
            print(f"{name} admission {admission:.2f} MiB")
        for name, record in run(args, root, {k: large[k] for k in ("webp", "tga")}, 512, 512, "rejected").items():
            assert record["error"]["code"] == "task_memory_exceeds_budget", (name, record)
        assert not list(root.glob("rejected-*.png"))

        small = {name: root / f"small.{name}" for name in writers}
        variants = dict(hdr=dict(mirrored=True, rle=False), dpx=dict(mirrored=True, little=False),
                        bmp=dict(top=True, rgba=True), webp=dict(orientation=6),
                        tga=dict(top=True, right=True))
        for name, path in small.items():
            writers[name](path, 42, 26, **variants[name])
        lossless = root / "lossless.webp"
        write_webp(lossless, 42, 26, lossless=True)
        for name, record in run(args, root, dict(small, lossless=lossless), 512, 512, "small").items():
            rotated = name == "webp"
            color = hdr_colors if name == "hdr" else [COLORS[i] for i in (2, 0, 3, 1)] if rotated else COLORS
            check(record, [(26, 42) if rotated else (42, 26)] * 2, color, 5)

        malformed = {name: root / f"bad.{name}" for name in writers}
        for name, path in malformed.items():
            path.write_bytes(small[name].read_bytes()[:12])
        for name, record in run(args, root, malformed, 512, 512, "malformed").items():
            assert record["error"]["code"] == "open_failed", (name, record)
        rejected = {"bmp-rle": root / "oversized.bmp", "tga-rle": root / "oversized.tga",
                    "webp-canvas": root / "oversized.webp", "webp-source": root / "large-source.webp",
                    "dpx-metadata": root / "large-header.dpx"}
        write_bmp(rejected["bmp-rle"], 12000, 6000, compression=1)
        write_tga(rejected["tga-rle"], 12000, 6000, rle=True)
        data = bytearray(small["webp"].read_bytes())
        canvas = data.index(b"VP8X") + 12
        data[canvas:canvas + 6] = (65535).to_bytes(3, "little") * 2
        rejected["webp-canvas"].write_bytes(data)
        with rejected["webp-source"].open("wb") as file:
            file.write(small["webp"].read_bytes())
            file.truncate(65 * 1024 * 1024)
        data = bytearray(small["dpx"].read_bytes())
        size = 5 * 1024 * 1024
        struct.pack_into(">I", data, 32, size)
        with rejected["dpx-metadata"].open("wb") as file:
            file.write(data)
            file.truncate(2048 + size)
        for name, record in run(args, root, rejected, 512, 64, "oversized").items():
            assert record["error"]["code"] == "decode_limit_exceeded", (name, record)
            assert "admission_mib" not in record["execution"], (name, record)


if __name__ == "__main__":
    main()
