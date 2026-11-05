"""Validate public manifests and local link targets; no input images required."""
import json
from pathlib import Path
import re

from jsonschema import Draft202012Validator


def main():
    root = Path(__file__).resolve().parents[2]
    schema = json.loads((root / "schemas/jobs-v1.schema.json").read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    manifests = sorted((root / "examples/public").glob("*.jobs.json"))
    if not manifests:
        raise ValueError("No public manifests found")
    for manifest in manifests:
        validator.validate(json.loads(manifest.read_text(encoding="utf-8")))
    documents = [root / "CHANGELOG.md", root / "examples/public/README.md"]
    documents += [root / "docs" / name for name in
                  ("getting-started.md", "public-assets.md", "public-reports.md")]
    links = 0
    for document in documents:
        for target in re.findall(r"\[[^\]]*\]\(([^\s)]+)\)", document.read_text(encoding="utf-8")):
            if "://" in target or target.startswith("#"):
                continue
            path = target.split("#", 1)[0]
            if not (document.parent / path).is_file():
                raise ValueError(f"Missing link target in {document.name}: {target}")
            links += 1
    print(f"Validated {len(manifests)} public manifests and {links} local link targets")


if __name__ == "__main__":
    main()
