"""CI dependency ordering and failure evidence without rebuilding native SDKs."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('product_deps', repo / 'scripts/ci/product-deps.py')
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)


class ProductDependencies(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / 'sdk'
        for name in ('sdk', 'oiio-deps/dist', 'oiio-deps/OpenEXR', 'oiio-deps/libjpeg-turbo'):
            (self.sdk / name).mkdir(parents=True)
        self.output = self.root / 'product'

    def test_ordered_prefixes_and_pinned_builders(self):
        result = deps.plan(repo, self.sdk, self.output, 'python-explicit')
        self.assertEqual(result['sdk_prefix'].split(';'), [str(self.output / 'colour/sdk'),
                         str(self.output / 'bounded-openexr/prefix'), str(self.sdk / 'sdk'), str(self.sdk / 'oiio-deps/dist')])
        self.assertTrue(all(cmd[0] == 'python-explicit' for _, cmd in result['commands']))
        self.assertIn('--jpeg-cache', result['commands'][0][1])
        self.assertIn('--source-cache', result['commands'][1][1])

    def test_reject_reuse_overlap_and_missing_source(self):
        for path in (self.sdk, self.sdk / 'nested', self.root):
            with self.subTest(path=path), self.assertRaises(ValueError):
                deps.plan(repo, self.sdk, path, 'python')
        (self.sdk / 'oiio-deps/OpenEXR').rmdir()
        with self.assertRaisesRegex(ValueError, 'Missing pinned SDK'):
            deps.plan(repo, self.sdk, self.output, 'python')
        self.assertFalse(self.output.exists())

    def test_failed_builder_keeps_original_exit_and_log_and_no_environment(self):
        def fail(command, **kwargs):
            kwargs['stdout'].write(b'original build failure\n')
            return subprocess.CompletedProcess(command, 17)
        with self.assertRaisesRegex(RuntimeError, 'colour builder failed: 17'):
            deps.prepare(repo, self.sdk, self.output, 'python', fail)
        record = json.loads((self.output / 'report.json').read_bytes())
        self.assertEqual(record['status'], 'failed')
        self.assertEqual(record['commands'][0]['returncode'], 17)
        self.assertEqual(len(record['commands']), 1)
        self.assertEqual((self.output / 'colour.log').read_bytes(), b'original build failure\n')
        self.assertFalse((self.output / 'environment.json').exists())

    def test_spawn_failure_and_missing_receipt_do_not_pass(self):
        def missing(command, **kwargs):
            raise FileNotFoundError('missing builder interpreter')
        with self.assertRaises(FileNotFoundError):
            deps.prepare(repo, self.sdk, self.output, 'python', missing)
        record = json.loads((self.output / 'report.json').read_bytes())
        self.assertIn('missing builder', record['commands'][0]['error'])
        with self.assertRaises(FileNotFoundError):
            deps.prepare(repo, self.sdk, self.root / 'no-receipt', 'python',
                         lambda command, **kwargs: subprocess.CompletedProcess(command, 0))
        self.assertEqual(json.loads((self.root / 'no-receipt/report.json').read_bytes())['status'], 'failed')

    def test_environment_emitted_only_after_both_receipts(self):
        def build(command, **kwargs):
            flag = '--root' if '--root' in command else '--output'
            directory = Path(command[command.index(flag) + 1])
            directory.mkdir()
            (directory / 'receipt.json').write_text('{}', encoding='utf-8')
            return subprocess.CompletedProcess(command, 0)
        env = deps.prepare(repo, self.sdk, self.output, 'python', build)
        report = json.loads((self.output / 'report.json').read_bytes())
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(len(report['commands']), 2)
        self.assertEqual(report['environment_sha256'], deps.digest(self.output / 'environment.json'))
        self.assertEqual(env['THUMBGEN_SDK_PREFIX'].split(';')[0], str(self.output / 'colour/sdk'))
        self.assertEqual(env['THUMBGEN_REQUIRE_BOUNDED_ZSTD'], '1')


if __name__ == '__main__':
    unittest.main()
