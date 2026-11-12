"""Validate public manifests and local link targets; no input images required."""
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

from jsonschema import Draft202012Validator


def public_documents(root):
    return [root / 'README.md', root / 'README.en.md', root / 'CHANGELOG.md',
            *sorted((root / 'docs').rglob('*.md')),
            *sorted((root / 'examples').rglob('*.md')),
            root / 'benchmarks/README.md', root / 'benchmarks/RESULTS.md', root / 'benchmarks/HISTORICAL.md', root / 'benchmarks/PRODUCTION_RESULTS.md',
            *sorted((root / 'benchmarks/results').glob('*/SUMMARY.md'))]


def validate_links(root, documents):
    root = root.resolve()
    links = 0
    for document in documents:
        if not document.is_file():
            raise ValueError(f'Missing public document: {document.name}')
        targets = re.findall(r'\[[^\]]*\]\((<[^>]+>|[^\s)]+)(?:\s+[\'"][^)]*[\'"])?\)',
                             document.read_text(encoding='utf-8'))
        for target in targets:
            target = target.removeprefix('<').removesuffix('>')
            scheme = urlsplit(target).scheme.lower()
            if scheme in ('https', 'http', 'mailto') or target.startswith('#'):
                continue
            path = unquote(target.split('#', 1)[0].split('?', 1)[0])
            resolved = (document.parent / path).resolve()
            if scheme or not resolved.is_relative_to(root):
                raise ValueError(f'Public link outside repository in {document.name}: {target}')
            relative = resolved.relative_to(root)
            if any(part in ('.deps', '.local', '.venv', 'build', '__pycache__') for part in relative.parts):
                raise ValueError(f'Public link requires private/generated files in {document.name}: {target}')
            if not resolved.exists():
                raise ValueError(f'Missing link target in {document.name}: {target}')
            links += 1
    return links


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
    documents = public_documents(root)
    links = validate_links(root, documents)
    print(f"Validated {len(manifests)} public manifests, {len(documents)} documents and {links} local link targets")


if __name__ == "__main__":
    main()
