"""Portable negative tests for independent Linux source/install provenance."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('runtime_overlays', Path(__file__).resolve().parents[2] / 'scripts/linux/runtime_overlays.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class Provenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.git('init', '-q')
        self.git('config', 'core.autocrlf', 'false')
        (self.source / 'source.c').write_bytes(b'int main(void) { return 0; }\n')
        (self.source / 'LICENSE').write_bytes(b'Authored test license\n')
        self.git('add', '.')
        self.git('-c', 'user.name=Package Tests', '-c', 'user.email=package-tests@example.invalid', 'commit', '-qm', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], text=True)

    def test_same_source_at_different_absolute_path(self):
        before = m.locked_tree(self.source, self.commit)
        moved = self.root / 'another-developer-root'
        self.source.rename(moved)
        self.assertEqual(before, m.locked_tree(moved, self.commit))

    def test_changed_source_refused(self):
        (self.source / 'source.c').write_text('unreviewed replacement')
        with self.assertRaisesRegex(ValueError, 'source bytes differ'):
            m.locked_tree(self.source, self.commit)

    def test_missing_notice_refused(self):
        (self.source / 'LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'source bytes differ'):
            m.locked_tree(self.source, self.commit)

    def test_untracked_source_refused(self):
        (self.source / 'injected.c').write_text('unreviewed')
        with self.assertRaisesRegex(ValueError, 'Untracked source'):
            m.locked_tree(self.source, self.commit)

    def test_wrong_commit_refused(self):
        with self.assertRaisesRegex(ValueError, 'commit differs'):
            m.locked_tree(self.source, '0' * 40)

    def test_forged_install_digest_is_not_sufficient(self):
        build, prefix = self.root / 'build', self.root / 'sdk'
        build.mkdir(); prefix.mkdir()
        (build / 'libCore.a').write_bytes(b'real build archive')
        (prefix / 'libCore.a').write_bytes(b'replacement archive')
        receipt = dict(installed_sha256={'libCore.a': m.sha(prefix / 'libCore.a')})
        proof = dict(installed_paths=[str(prefix / 'libCore.a')])
        with self.assertRaisesRegex(ValueError, 'not the recorded build product'):
            m.installed_product(build, prefix, 'libCore.a', receipt, proof)

    def test_absent_install_manifest_entry_refused(self):
        build, prefix = self.root / 'build', self.root / 'sdk'
        build.mkdir(); prefix.mkdir()
        for root in (build, prefix):
            (root / 'libCore.a').write_bytes(b'same bytes')
        receipt = dict(installed_sha256={'libCore.a': m.sha(prefix / 'libCore.a')})
        with self.assertRaisesRegex(ValueError, 'absent from CMake manifest'):
            m.installed_product(build, prefix, 'libCore.a', receipt, dict(installed_paths=[]))

    def test_bad_installed_digest_refused(self):
        (self.root / 'libCore.a').write_bytes(b'archive')
        with self.assertRaisesRegex(ValueError, 'Installed digest differs'):
            m.installed_product(self.root, self.root, 'libCore.a', dict(installed_sha256={'libCore.a': '0' * 64}), {})


if __name__ == '__main__':
    unittest.main()
