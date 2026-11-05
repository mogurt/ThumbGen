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
        for directory in ("docs", "examples", "schemas", "scripts", "tests", "benchmarks"):
            shutil.copytree(ROOT / directory, self.root / directory,
                            ignore=shutil.ignore_patterns("__pycache__", "results", "data", "output"))
        (self.root / "tests/data").mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "tests/data/deep-reference-v1.json",
                        self.root / "tests/data/deep-reference-v1.json")
        shutil.copyfile(ROOT / "CHANGELOG.md", self.root / "CHANGELOG.md")

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


if __name__ == "__main__":
    unittest.main()
