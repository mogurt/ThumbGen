"""Public build provenance boundaries: real Git trees and native evidence maps."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


policy = module('policy', 'scripts/dependencies/windows_overlays.py')
proof = module('proof', 'scripts/dependencies/overlay_provenance.py')


class ProvenanceBoundaries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.PIPE)

    def commit(self):
        self.git('add', '.')
        self.git('-c', 'user.name=Packaging test', '-c', 'user.email=local@example.invalid', 'commit', '-qm', 'fixture')
        return self.git('rev-parse', 'HEAD').decode().strip()

    def source(self):
        self.git('init', '-q')
        (self.root / 'source.c').write_bytes(b'int original;\n')
        return self.commit()

    def test_exact_source_tree_and_skip_worktree_tamper(self):
        commit = self.source()
        self.assertEqual(proof.source_tree(policy, self.root, commit)['commit'], commit)
        self.git('update-index', '--skip-worktree', 'source.c')
        (self.root / 'source.c').write_bytes(b'int injected;\n')
        with self.assertRaisesRegex(ValueError, 'Source content differs'):
            proof.source_tree(policy, self.root, commit)

    def test_untracked_source_injection(self):
        commit = self.source()
        (self.root / 'injected.h').write_bytes(b'int injected;')
        with self.assertRaisesRegex(ValueError, 'Untracked or ignored'):
            proof.source_tree(policy, self.root, commit)

    def test_ignored_source_injection(self):
        self.source(); (self.root / '.gitignore').write_text('ignored.h\n')
        commit = self.commit(); (self.root / 'ignored.h').write_bytes(b'int injected;')
        with self.assertRaisesRegex(ValueError, 'Untracked or ignored'):
            proof.source_tree(policy, self.root, commit)

    def test_trusted_patch_exact_context(self):
        patch_bytes = b'diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-old\n+new\n'
        self.assertEqual(proof.patched_blobs({'a.c': b'old\n'}, patch_bytes), {'a.c': b'new\n'})
        with self.assertRaisesRegex(ValueError, 'context differs'):
            proof.patched_blobs({'a.c': b'other\n'}, patch_bytes)

    def test_trusted_helper_evolves_without_per_install_pin(self):
        self.git('init', '-q')
        paths = ['scripts/dependencies/build_verified_overlay.py', 'scripts/dependencies/overlay_provenance.py',
                 'scripts/dependencies/windows_overlays.py', 'dependencies/color-sdk.json', 'scripts/build-color-sdk.py']
        for relative in paths:
            path = self.root / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('first\n')
        self.commit()
        with patch.object(policy, 'REPO', self.root):
            before = proof.trusted_recipes(policy, 'color')
            (self.root / paths[-1]).write_text('new reviewed helper\n')
            with self.assertRaisesRegex(ValueError, 'Uncommitted provenance recipe'):
                proof.trusted_recipes(policy, 'color')
            self.commit()
            after = proof.trusted_recipes(policy, 'color')
        self.assertNotEqual(before[-1]['lf_sha256'], after[-1]['lf_sha256'])

    def test_cache_options_and_launcher_injection(self):
        values = dict(CMAKE_HOME_DIRECTORY=self.root.as_posix(), CMAKE_INSTALL_PREFIX=self.root.as_posix(),
                      CMAKE_GENERATOR='Visual Studio 17 2022', CMAKE_GENERATOR_PLATFORM='x64', ENABLE_SHARED='ON')
        proof.cache_check(values, self.root, self.root, {'ENABLE_SHARED': 'ON'})
        with self.assertRaisesRegex(ValueError, 'option'):
            proof.cache_check(dict(values, ENABLE_SHARED='OFF'), self.root, self.root, {'ENABLE_SHARED': 'ON'})
        with self.assertRaisesRegex(ValueError, 'injection'):
            proof.cache_check(dict(values, CMAKE_C_COMPILER_LAUNCHER='injected.exe'), self.root, self.root, {})

    def install_fixture(self):
        source, build, prefix = [self.root / n for n in ('source', 'build', 'sdk')]
        for path in (source, build, prefix / 'bin'):
            path.mkdir(parents=True)
        origin, target = build / 'a.dll', prefix / 'bin/a.dll'
        origin.write_bytes(b'native bytes'); target.write_bytes(origin.read_bytes())
        trace = self.root / 'install.jsonl'
        trace.write_text(json.dumps(dict(cmd='file', args=['INSTALL', 'DESTINATION', str(target.parent),
            'TYPE', 'SHARED_LIBRARY', 'FILES', str(origin)])) + '\n')
        return source, build, prefix, origin, target, trace

    def test_install_mapping_uses_native_origin_bytes(self):
        source, build, prefix, origin, target, trace = self.install_fixture()
        self.assertEqual(proof.install_map(policy, trace, source, build, prefix)['bin/a.dll']['origin'], str(origin))
        target.write_bytes(b'forged installation')
        with self.assertRaisesRegex(ValueError, 'native origin'):
            proof.install_map(policy, trace, source, build, prefix)

    def test_install_origin_escape_is_rejected(self):
        source, build, prefix, origin, target, trace = self.install_fixture()
        outside = self.root / 'injected.dll'; outside.write_bytes(origin.read_bytes())
        trace.write_text(trace.read_text().replace(str(origin).replace('\\', '\\\\'), str(outside).replace('\\', '\\\\')))
        with self.assertRaisesRegex(ValueError, 'Untrusted native install origin'):
            proof.install_map(policy, trace, source, build, prefix)

    def test_unknown_provenance_profile_is_not_generic_receipt(self):
        prefix = self.root / 'sdk'; prefix.mkdir()
        (self.root / 'receipt.json').write_text('{"installed_sha256":{}}')
        (self.root / 'overlay-provenance.json').write_text('{"kind":"generic","installed_sha256":{}}')
        with self.assertRaisesRegex(ValueError, 'Unknown overlay provenance'):
            policy.verify('color', prefix)

    def test_forged_profile_without_actual_native_tree_fails(self):
        prefix = self.root / 'sdk'; prefix.mkdir()
        (self.root / 'receipt.json').write_text('{"installed_sha256":{}}')
        (self.root / 'overlay-provenance.json').write_text('{"kind":"thumbgen-overlay-build-v2","overlay":"color"}')
        with self.assertRaises((OSError, ValueError, subprocess.CalledProcessError)):
            policy.verify('color', prefix)

    def test_malformed_native_tracking_log_fails(self):
        file = self.root / 'CL.read.1.tlog'; file.write_text('unowned-input.h\n', encoding='utf-16')
        with self.assertRaisesRegex(ValueError, 'Malformed native'):
            proof.tlog_records(file)


if __name__ == '__main__':
    unittest.main()
