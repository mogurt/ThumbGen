"""Assemble the public ThumbGen runtime layout from audited platform builds."""

import argparse
import gzip
import hashlib
import importlib
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(stage, name, data):
    path = stage / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def linux_payload(args, stage):
    with tarfile.open(args.candidate) as candidate:
        roots = {member.name.split("/", 1)[0] for member in candidate.getmembers()}
        if len(roots) != 1:
            raise ValueError("Expected a single-root Linux candidate")
        prefix = roots.pop() + "/"
        provenance = json.load(candidate.extractfile(prefix + "DEPENDENCIES.json"))
        if (provenance["frozen_product_commit"] != args.commit
                or provenance["identity"]["version"] != args.version):
            raise ValueError("Linux candidate commit or version mismatch")
        for member in candidate.getmembers():
            path = member.name.removeprefix(prefix)
            if not path.startswith(("bin/", "lib/", "licenses/")):
                continue
            if member.isfile():
                write(stage, path, candidate.extractfile(member).read())
                (stage / path).chmod(member.mode)
            elif member.issym():
                (stage / path).parent.mkdir(parents=True, exist_ok=True)
                (stage / path).symlink_to(member.linkname)

    components = [{k: v for k, v in component.items() if k not in ("source", "source_path")}
                  for component in provenance["dependency_components"]]
    return {"components": components, "runtime": {
        path.relative_to(stage).as_posix(): digest(path)
        for folder in ("bin", "lib") for path in sorted((stage / folder).iterdir())
        if path.is_file() and not path.is_symlink()
    }}


def windows_payload(args, stage):
    sys.path.insert(0, str(args.source / "scripts/windows"))
    audit = importlib.import_module("runtime_closure").audit
    closure = audit(args.install / "bin/thumbgen.exe", args.sdk,
                    Path(os.environ["SystemRoot"]) / "System32", args.color, args.bounded)
    if digest(args.candidate) != "5ab79eca211f08c498ded59ff3873515245b90976c868a09f5ee05b70d48819f":
        raise ValueError("Published Windows notice bundle checksum mismatch")
    with zipfile.ZipFile(args.candidate) as candidate:
        prior = json.loads(candidate.read("ThumbGen/DEPENDENCIES.json"))
        for member in candidate.infolist():
            path = member.filename.removeprefix("ThumbGen/")
            if not member.is_dir() and path.startswith(("licenses/", "provenance/patches/")):
                write(stage, path, candidate.read(member))
    for item in closure["files"]:
        target = stage / "bin" / item["name"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(item["source_path"], target)
        if digest(target) != item["sha256"]:
            raise ValueError(f"Windows dependency changed during staging: {item['name']}")
    return {"components": prior["components"], "runtime": [
        {key: item[key] for key in ("name", "sha256")} for item in closure["files"]
    ]}


def archive(stage, output, platform):
    paths = sorted(path for path in stage.rglob("*") if path.is_file() or path.is_symlink())
    if platform == "windows":
        with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as package:
            for path in paths:
                package.write(path, "ThumbGen/" + path.relative_to(stage).as_posix())
    else:
        with output.open("xb") as stream, gzip.GzipFile(fileobj=stream, filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as package:
                for path in paths:
                    info = package.gettarinfo(str(path), "ThumbGen/" + path.relative_to(stage).as_posix())
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    if path.is_symlink():
                        package.addfile(info)
                    else:
                        with path.open("rb") as content:
                            package.addfile(info, content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("windows", "linux"))
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    for name in ("source", "install", "candidate", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("sdk", "color", "bounded"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        stage = Path(directory) / "ThumbGen"
        stage.mkdir()
        if args.platform == "windows":
            metadata = windows_payload(args, stage)
            suffix = "windows-x64.zip"
        else:
            metadata = linux_payload(args, stage)
            suffix = "linux-x86_64-glibc217.tar.gz"
        shutil.copytree(args.install / "share", stage / "share", dirs_exist_ok=True)
        shutil.copyfile(args.source / "LICENSE", stage / "LICENSE")
        write(stage, "NOTICE", b"ThumbGen is Apache-2.0. Third-party notices are in licenses/ and share/ThumbGen/notices/.\n")
        write(stage, "README.txt", (f"ThumbGen {args.version}\n\nKeep this directory together.\n"
                                     "Run bin/thumbgen --version --json to check the installation.\n").encode())
        metadata.update(version=args.version, commit=args.commit, c2_png_stream=False)
        write(stage, "DEPENDENCIES.json", (json.dumps(metadata, indent=2) + "\n").encode())
        checksums = [f"{digest(path)}  {path.relative_to(stage).as_posix()}"
                     for path in sorted(stage.rglob("*")) if path.is_file() and not path.is_symlink()]
        write(stage, "SHA256SUMS", ("\n".join(checksums) + "\n").encode())
        args.output.mkdir(parents=True, exist_ok=True)
        archive(stage, args.output / f"thumbgen-{args.version}-{suffix}", args.platform)


if __name__ == "__main__":
    main()
