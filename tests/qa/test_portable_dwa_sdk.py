"""Preflight rejection tests; no SDK builds or modifications are performed."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HELPER = Path(__file__).resolve().parents[2] / 'scripts/dependencies/build_portable_dwa_sdk.py'
spec = importlib.util.spec_from_file_location('portable_dwa_builder', HELPER)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class SourceBoundary(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / 'cache'
        self.cache.mkdir()
        self.git('init', '-q')
        self.git('config', 'core.autocrlf', 'false')
        self.git('config', 'user.name', 'QA')
        self.git('config', 'user.email', 'qa@example.invalid')
        (self.cache / 'source.h').write_bytes(b'original\n')
        (self.cache / '.gitignore').write_bytes(b'*.ignored\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD').decode().strip()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.cache), *args], stderr=subprocess.STDOUT)

    def test_clean_exact_source_is_accepted(self):
        self.assertIn(b'source.h', builder.verify_cache(self.cache, self.commit))

    def test_wrong_commit_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'HEAD'):
            builder.verify_cache(self.cache, '0' * 40)

    def test_modified_tracked_source_is_rejected(self):
        (self.cache / 'source.h').write_bytes(b'changed\n')
        with self.assertRaisesRegex(ValueError, 'changed'):
            builder.verify_cache(self.cache, self.commit)

    def test_additional_ignored_source_is_rejected(self):
        (self.cache / 'extra.ignored').write_bytes(b'unknown\n')
        with self.assertRaisesRegex(ValueError, 'additional'):
            builder.verify_cache(self.cache, self.commit)

    def test_existing_output_is_rejected_without_mutation(self):
        output = self.root / 'existing'
        output.mkdir()
        marker = output / 'old-receipt'
        marker.write_bytes(b'preserve')
        args = ['builder', '--openexr-cache', str(self.cache), '--oiio-cache', str(self.cache),
                '--deps-prefix', str(self.cache), '--output', str(output)]
        with patch.object(builder.sys, 'platform', 'linux'), patch.object(builder.sys, 'argv', args):
            with self.assertRaises(SystemExit) as error:
                builder.main()
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(marker.read_bytes(), b'preserve')

    def test_checkout_private_ci_output_reaches_source_verification(self):
        checkout = self.root / 'project'
        checkout.mkdir()
        output = checkout / '.deps/ci-product-deps/portable-dwa'
        args = ['builder', '--openexr-cache', str(self.cache), '--oiio-cache', str(self.cache),
                '--deps-prefix', str(self.cache), '--output', str(output)]
        with patch.object(builder, 'ROOT', checkout), patch.object(builder.sys, 'platform', 'linux'), \
                patch.object(builder.sys, 'argv', args), \
                patch.object(builder, 'verify_cache', side_effect=RuntimeError('source verification reached')):
            with self.assertRaisesRegex(RuntimeError, 'source verification reached'):
                builder.main()
        self.assertFalse(output.exists())

    def test_tracked_source_destination_and_nested_cache_refused(self):
        checkout = self.root / 'project'
        cache = checkout / '.deps/cache'
        cache.mkdir(parents=True)
        for output in (checkout / 'scripts/new', checkout / 'new', cache / 'nested'):
            with self.subTest(output=output), self.assertRaises(ValueError):
                builder.output_location(output, [cache], checkout)
            self.assertFalse(output.exists())

    def test_external_private_output_still_allowed(self):
        checkout = self.root / 'project'
        checkout.mkdir()
        output = self.root / 'separate-build'
        self.assertEqual(builder.output_location(output, [self.cache], checkout), output)
        self.assertFalse(output.exists())

    def test_private_symlink_into_source_is_not_authorized(self):
        checkout = self.root / 'project'
        source = checkout / 'src'
        source.mkdir(parents=True)
        try:
            (checkout / '.deps').symlink_to(source, target_is_directory=True)
        except OSError as error:
            self.skipTest('Directory symlinks unavailable: ' + str(error))
        output = checkout / '.deps/new'
        with self.assertRaises(ValueError):
            builder.output_location(output, [self.cache], checkout)
        self.assertFalse((source / 'new').exists())


if __name__ == '__main__':
    unittest.main()
