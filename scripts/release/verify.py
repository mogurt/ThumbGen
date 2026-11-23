"""Verify a public archive and exercise it after a fresh extraction."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import struct
import subprocess
import tarfile
import tempfile
import zipfile
import zlib


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(package, directory):
    with (zipfile.ZipFile(package) if package.suffix == ".zip" else tarfile.open(package)) as archive:
        names = archive.namelist() if package.suffix == ".zip" else archive.getnames()
        for name in names:
            path = PurePosixPath(name)
            if not path.parts or path.parts[0] != "ThumbGen" or ".." in path.parts or "\\" in name or ":" in name:
                raise ValueError(f"Invalid package member {name}")
        if len(names) != len(set(names)):
            raise ValueError("Duplicate package members")
        if isinstance(archive, tarfile.TarFile):
            for member in archive.getmembers():
                if member.issym():
                    target = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), member.linkname))
                    if not target.startswith("ThumbGen/"):
                        raise ValueError(f"Package link escapes ThumbGen: {member.name}")
                elif not (member.isfile() or member.isdir()):
                    raise ValueError(f"Unexpected package member: {member.name}")
            archive.extractall(directory, filter="data")
        else:
            archive.extractall(directory)


def verify(package, version, commit):
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        extract(package, scratch)
        root = scratch / "ThumbGen"
        metadata = json.loads((root / "DEPENDENCIES.json").read_text(encoding="utf-8"))
        if metadata["version"] != version or metadata["commit"] != commit:
            raise ValueError("Package metadata mismatch")
        expected = dict(line.rsplit("  ", 1)[::-1] for line in (root / "SHA256SUMS").read_text().splitlines())
        actual = {p.relative_to(root).as_posix(): digest(p) for p in root.rglob("*")
                  if p.is_file() and not p.is_symlink() and p != root / "SHA256SUMS"}
        if actual != expected:
            raise ValueError("Package checksum mismatch")

        env = dict(os.environ)
        for key in ("LD_LIBRARY_PATH", "OCIO"):
            env.pop(key, None)
        env["PATH"] = str(Path(env["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin"
        exe = root / "bin" / ("thumbgen.exe" if os.name == "nt" else "thumbgen")

        def probe(*args):
            return json.loads(subprocess.check_output([str(exe), *args, "--json"], env=env, timeout=30))

        identity = probe("--version")
        capabilities = probe("capabilities")
        if (identity["version"] != version or identity["build"]["dirty"]
                or identity["build"]["revision"] not in ("unknown", commit[:12])
                or "auto" not in capabilities["visualization_modes"]
                or "zstd" not in capabilities["deep_compressions"]):
            raise ValueError("Packaged executable differs from expected product")

        def chunk(name, data):
            return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

        source = scratch / "input.png"
        source.write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"\x00\xff\x80\x40"))
                           + chunk(b"IEND", b""))
        output = scratch / "thumbnail.jpg"
        subprocess.run([str(exe), "convert", str(source), "--output", str(output), "--visualize", "auto"],
                       env=env, check=True, timeout=30)
        if not output.read_bytes().startswith(b"\xff\xd8"):
            raise ValueError("Automatic preview did not produce a JPEG")
        print(json.dumps(identity))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    verify(args.package, args.version, args.commit)
