"""Exercise deferred JPEG decode through the public batch controller."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

import jsonschema

parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--fixture-exe", type=Path, required=True)
parser.add_argument("--runtime-dir", type=Path, required=True)
parser.add_argument("--schema-dir", type=Path, required=True)
args = parser.parse_args()
env = dict(os.environ)
env.pop("OCIO", None)
key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
env[key] = str(args.runtime_dir.resolve()) + os.pathsep + env.get(key, "")
validator = jsonschema.Draft202012Validator(
    json.loads((args.schema_dir / "protocol-v1.schema.json").read_text(encoding="utf-8")))


def run(root, sources, memory, prefix):
    jobs = [
        dict(id=name, input=str(path), outputs=[
            dict(path=str(root / f"{prefix}-{name}-{width}.png"), size=[width, height], format="png")
            for width, height in ((1024, 800), (360, 280))])
        for name, path in sources.items()
    ]
    manifest = dict(schema_version=1, defaults=dict(threads=1), jobs=jobs,
                    execution=dict(workers=1, thread_budget=1, memory_budget_mib=memory))
    path = root / "jobs.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    completed = subprocess.run([str(args.exe.resolve()), "batch", "--manifest", str(path), "--json"],
                               env=env, capture_output=True, text=True, timeout=180)
    rows = [json.loads(line) for line in completed.stdout.splitlines()]
    for row in rows:
        validator.validate(row)
    results = {r["id"]: r for r in rows if r["type"] == "result"}
    assert results.keys() == sources.keys(), (completed.stdout, completed.stderr)
    return results


def oversized_frame(small, progressive):
    buf = bytearray(small)
    frame = buf.index(b"\xff\xc2" if progressive else b"\xff\xc0")
    buf[frame + 5:frame + 9] = struct.pack(">HH", 24000, 24000)
    if progressive:
        return buf
    # A sequential JPEG with a noninterleaved first scan also retains full
    # coefficients. Its declared cost must be rejected before entropy decode.
    scan = buf.index(b"\xff\xda")
    return buf[:scan] + b"\xff\xda\x00\x08\x01" + buf[scan + 5:scan + 7] + b"\x00\x3f\x00\xff\xd9"


def png_size(path):
    with Path(path).open("rb") as preview:
        header = preview.read(24)
    assert header[:16] == b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR", path
    return struct.unpack(">II", header[16:])


with tempfile.TemporaryDirectory(prefix="bounded-jpeg-") as tmp:
    root = Path(tmp)
    subprocess.run([str(args.fixture_exe.resolve()), "--fixtures", str(root)], env=env, check=True, timeout=90)
    sources = {name: root / f"{name}.jpg" for name in
               ("baseline", "baseline12", "progressive", "oversized", "multiscan", "truncated", "metadata")}
    for name, progressive in (("oversized", True), ("multiscan", False)):
        small = root / ("small-progressive.jpg" if progressive else "small-baseline.jpg")
        sources[name].write_bytes(oversized_frame(small.read_bytes(), progressive))
    sources["truncated"].write_bytes(b"\xff\xd8\xff\xe1\x00\x25Exif\0\0")
    with sources["metadata"].open("wb") as handle:
        handle.write(b"\xff\xd8")
        for _ in range(129):
            handle.write(b"\xff\xe1\xff\xff" + bytes(65533))
    results = run(root, sources, 2048, "admitted")
    for name, heights in (("baseline", (512, 180)), ("baseline12", (683, 240)), ("progressive", (512, 180))):
        record = results[name]
        assert record["ok"] and record["execution"]["admission_mib"] < 512, record
        for output, size in zip(record["outputs"], zip((1024, 360), heights)):
            assert png_size(output["path"]) == size, output
        print(f'{name} admission {record["execution"]["admission_mib"]:.2f} MiB')
    assert results["progressive"]["colour"]["icc"]["engine"] == "skcms", results["progressive"]
    for name in ("oversized", "multiscan"):
        record = results[name]
        assert record["error"]["code"] == "decode_limit_exceeded" and "admission_mib" not in record["execution"], record
    for name in ("truncated", "metadata"):
        assert results[name]["error"]["code"] == "open_failed", results[name]
    rejected = run(root, {"progressive": sources["progressive"]}, 320, "rejected")["progressive"]
    assert rejected["error"]["code"] == "task_memory_exceeds_budget", rejected
    assert not list(root.glob("rejected-*.png"))
