"""CI dependency ordering and failure evidence without rebuilding native SDKs."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
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
        self.root = Path(self.temp.name).resolve()
        self.sdk = self.root / 'sdk'
        for name in ('sdk', 'oiio-source', 'oiio-deps/dist', 'oiio-deps/OpenEXR', 'oiio-deps/libjpeg-turbo'):
            (self.sdk / name).mkdir(parents=True)
        self.output = self.root / 'product'

    def test_ordered_prefixes_and_pinned_builders(self):
        result = deps.plan(repo, self.sdk, self.output, 'python-explicit')
        self.assertEqual(result['sdk_prefix'].split(';'), [str(self.output / 'colour/sdk'),
                         str(self.output / 'bounded-openexr/prefix'), str(self.output / 'portable-dwa/prefix'),
                         str(self.sdk / 'sdk'), str(self.sdk / 'oiio-deps/dist')])
        self.assertTrue(all(cmd[0] == 'python-explicit' for _, cmd in result['commands']))
        self.assertIn('--jpeg-cache', result['commands'][0][1])
        self.assertIn('--source-cache', result['commands'][1][1])
        dwa = result['commands'][2][1]
        self.assertEqual(dwa[dwa.index('--oiio-cache') + 1], str(self.sdk / 'oiio-source'))
        self.assertEqual(dwa[dwa.index('--openexr-cache') + 1], str(self.sdk / 'oiio-deps/OpenEXR'))

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

    def test_environment_emitted_only_after_all_receipts(self):
        def build(command, **kwargs):
            flag = '--root' if '--root' in command else '--output'
            directory = Path(command[command.index(flag) + 1])
            directory.mkdir()
            (directory / 'receipt.json').write_text('{}', encoding='utf-8')
            return subprocess.CompletedProcess(command, 0)
        env = deps.prepare(repo, self.sdk, self.output, 'python', build)
        report = json.loads((self.output / 'report.json').read_bytes())
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(len(report['commands']), 3)
        self.assertEqual(report['portable-dwa_receipt_sha256'], deps.digest(self.output / 'portable-dwa/receipt.json'))
        for path in ('dependencies/patches/openexr-portable-dwa-v1.patch',
                     'scripts/dependencies/build_portable_dwa_sdk.py'):
            self.assertEqual(report['inputs_sha256'][path], deps.digest(repo / path))
        self.assertEqual(report['environment_sha256'], deps.digest(self.output / 'environment.json'))
        self.assertEqual(env['THUMBGEN_SDK_PREFIX'].split(';')[0], str(self.output / 'colour/sdk'))
        self.assertEqual(env['THUMBGEN_REQUIRE_BOUNDED_ZSTD'], '1')

    def test_dwa_failure_preserves_prior_receipts_without_emitting_environment(self):
        def build(command, **kwargs):
            if '--oiio-cache' in command:
                kwargs['stdout'].write(b'original DWA failure\n')
                return subprocess.CompletedProcess(command, 23)
            flag = '--root' if '--root' in command else '--output'
            directory = Path(command[command.index(flag) + 1])
            directory.mkdir()
            (directory / 'receipt.json').write_text('{}', encoding='utf-8')
            return subprocess.CompletedProcess(command, 0)
        with self.assertRaisesRegex(RuntimeError, 'portable-dwa builder failed: 23'):
            deps.prepare(repo, self.sdk, self.output, 'python', build)
        report = json.loads((self.output / 'report.json').read_bytes())
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['commands'][-1]['returncode'], 23)
        self.assertIn('colour_receipt_sha256', report)
        self.assertIn('bounded-openexr_receipt_sha256', report)
        self.assertNotIn('portable-dwa_receipt_sha256', report)
        self.assertEqual((self.output / 'portable-dwa.log').read_bytes(), b'original DWA failure\n')
        self.assertFalse((self.output / 'environment.json').exists())

    def test_missing_oiio_source_rejected_before_creating_output(self):
        (self.sdk / 'oiio-source').rmdir()
        with self.assertRaisesRegex(ValueError, 'Missing pinned SDK input: oiio-source'):
            deps.plan(repo, self.sdk, self.output, 'python')
        self.assertFalse(self.output.exists())

    @unittest.skipUnless(shutil.which('cmake'), 'CMake required for actual prefix selection')
    def test_cmake_selects_portable_oiio_and_separate_bounded_core(self):
        result = deps.plan(repo, self.sdk, self.output, 'python')
        bounded = self.output / 'bounded-openexr/prefix'
        portable = self.output / 'portable-dwa/prefix'
        for prefix, package, marker in ((bounded, 'OpenEXR', 'bounded'),
                                        (portable, 'OpenEXR', 'flat-static'),
                                        (portable, 'OpenImageIO', 'portable'),
                                        (self.sdk / 'sdk', 'OpenImageIO', 'original')):
            directory = prefix / 'lib/cmake' / package
            directory.mkdir(parents=True)
            (directory / (package + 'Config.cmake')).write_text(
                f'set(SELECTED_{package} "{marker}")\n', encoding='utf-8')
        source = self.root / 'consumer'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(prefix_order NONE)\n'
            'find_package(OpenImageIO CONFIG REQUIRED)\nfind_package(OpenEXR CONFIG REQUIRED)\n'
            'if(NOT SELECTED_OpenEXR STREQUAL "bounded" OR NOT SELECTED_OpenImageIO STREQUAL "portable")\n'
            'message(FATAL_ERROR "Wrong dependency selected")\nendif()\n', encoding='utf-8')
        command = ['cmake', '-S', str(source), '-B', str(self.root / 'consumer-build'),
                   '-DCMAKE_PREFIX_PATH=' + result['sdk_prefix'],
                   '-DCMAKE_FIND_USE_PACKAGE_ROOT_PATH=OFF', '-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF',
                   '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
        proc = subprocess.run(command, capture_output=True, text=True, env=dict(os.environ))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == '__main__':
    unittest.main()
