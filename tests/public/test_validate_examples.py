"""Exercise failure detection without changing the checked-in examples."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PublicValidationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        # Copy referenced files so this tests the same link surface as the real run.
        for directory in ("docs", "examples", "schemas", "scripts", "tests", "benchmarks",
                          "src", "include", "cmake", "dependencies", ".github"):
            shutil.copytree(ROOT / directory, self.root / directory,
                            ignore=shutil.ignore_patterns("__pycache__", "results", "data", "output"))
        (self.root / "tests/data").mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "tests/data/deep-reference-v1.json",
                        self.root / "tests/data/deep-reference-v1.json")
        shutil.copyfile(ROOT / "CHANGELOG.md", self.root / "CHANGELOG.md")
        shutil.copyfile(ROOT / "README.md", self.root / "README.md")
        shutil.copyfile(ROOT / "README.zh-CN.md", self.root / "README.zh-CN.md")
        for name in ('CMakeLists.txt', 'CMakePresets.json'):
            shutil.copyfile(ROOT / name, self.root / name)
        for summary in (ROOT / 'benchmarks/results').glob('*/SUMMARY.md'):
            target = self.root / summary.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(summary, target)

    def run_validator(self):
        return subprocess.run([sys.executable, str(self.root / "scripts/public/validate_examples.py")],
                              capture_output=True, text=True, encoding="utf-8")

    def test_valid_examples(self):
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_manifest_field_is_rejected(self):
        path = self.root / "examples/public/colour.jobs.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["jobs"][0]["unrecognized"] = True
        path.write_text(json.dumps(manifest), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Additional properties are not allowed", result.stderr)

    def test_missing_local_target_is_rejected(self):
        (self.root / "docs/selection.md").unlink()
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing link target", result.stderr)

    def test_root_readme_and_new_documents_are_checked(self):
        for path in ('README.md', 'README.zh-CN.md', 'docs/future-feature.md', 'benchmarks/results/new-summary/SUMMARY.md'):
            with self.subTest(path=path):
                target = self.root / path
                original = target.read_bytes() if target.exists() else None
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('[missing](missing-public-file.md)', encoding='utf-8')
                result = self.run_validator()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Missing link target', result.stderr)
                if original is None:
                    target.unlink()
                else:
                    target.write_bytes(original)

    def test_existing_private_generated_target_cannot_satisfy_public_link(self):
        for directory in ('.deps', '.local'):
            with self.subTest(directory=directory):
                target = self.root / directory / 'private-report.json'
                target.parent.mkdir()
                target.write_text('{}', encoding='utf-8')
                (self.root / 'docs/future-feature.md').write_text(
                    f'[report](../{directory}/private-report.json)', encoding='utf-8')
                result = self.run_validator()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('private/generated', result.stderr)

    def test_outside_and_file_uri_targets_are_rejected(self):
        for target in ('../../outside-report.md', 'file:///private/report.json'):
            with self.subTest(target=target):
                (self.root / 'docs/future-feature.md').write_text(f'[report]({target})', encoding='utf-8')
                result = self.run_validator()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('outside repository', result.stderr)

    def test_encoded_and_angle_bracket_paths_with_spaces(self):
        (self.root / 'docs/space target.md').write_text('text', encoding='utf-8')
        (self.root / 'docs/future-feature.md').write_text('[one](space%20target.md) [two](<space target.md>)', encoding='utf-8')
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
