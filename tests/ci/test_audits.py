"""Regression checks for CI accepting incomplete or contaminated results."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys

spec = importlib.util.spec_from_file_location('ci_run', Path(__file__).resolve().parents[2] / 'scripts/ci/run.py')
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class Audits(unittest.TestCase):
    def test_bounded_runtime_is_required_when_ci_selects_it(self):
        base = dict(type='capabilities', deep_compressions=['none', 'rle', 'zips'])
        self.assertEqual(ci.audit_capabilities(json.dumps(base)), base['deep_compressions'])
        with self.assertRaisesRegex(RuntimeError, 'bounded ZSTD runtime'):
            ci.audit_capabilities(json.dumps(base), True)
        base['deep_compressions'].append('zstd')
        self.assertIn('zstd', ci.audit_capabilities(json.dumps(base), True))
        for bad in ('', '{}', json.dumps(base) + '\n' + json.dumps(base), '{"type":"capabilities"}'):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                ci.audit_capabilities(bad)

    def setUp(self):
        self.names = ['public_cli', 'public_protocol', 'public_interrupt', 'public_aov', 'public_color_input',
                      'public_cryptomatte', 'public_sampled_id', 'public_deep', 'core_deep_flatten', 'core_deep_reader', 'core_deep_output', 'core_deep_selection', 'core_deep_runtime', 'core_deep_infinite', 'core_deep_zstd', 'core_selected_input', 'core_cms', 'future_test']
        self.inventory = {'tests': [dict(name=name) for name in self.names]}

    def xml(self, names=None, status=''):
        return '<testsuite>' + ''.join(f'<testcase name="{name}">{status}</testcase>'
                                      for name in (self.names if names is None else names)) + '</testsuite>'

    def test_all_current_and_future_tests_are_required(self):
        self.assertEqual(ci.audit_tests(self.inventory, self.xml()), len(self.names))
        with self.assertRaisesRegex(RuntimeError, 'entire registered'):
            ci.audit_tests(self.inventory, self.xml(self.names[:-1]))

    def test_duplicate_junit_result_cannot_replace_test(self):
        with self.assertRaisesRegex(RuntimeError, 'entire registered'):
            ci.audit_tests(self.inventory, self.xml(self.names[:-1] + [self.names[0]]))

    def test_skips_and_failures_are_not_green(self):
        for status in ('<skipped/>', '<failure/>', '<error/>'):
            with self.subTest(status=status), self.assertRaisesRegex(RuntimeError, 'skipped'):
                ci.audit_tests(self.inventory, self.xml(status=status))

    def test_missing_public_test_registration_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'registration'):
            ci.audit_tests({'tests': self.inventory['tests'][1:]}, self.xml(self.names[1:]))

    def test_reader_cannot_be_omitted_from_inventory_and_results(self):
        for missing in ('core_deep_reader', 'core_deep_output', 'core_deep_selection', 'core_deep_runtime', 'core_deep_infinite', 'core_deep_zstd', 'core_selected_input', 'core_cms', 'public_deep', 'public_color_input'):
            names = [name for name in self.names if name != missing]
            with self.subTest(missing=missing), self.assertRaisesRegex(RuntimeError, 'registration'):
                ci.audit_tests({'tests': [dict(name=name) for name in names]}, self.xml(names))

    def test_disabled_junit_attributes_are_rejected(self):
        for status in ('notrun', 'disabled'):
            junit = self.xml().replace('<testcase ', '<testcase status="' + status + '" ', 1)
            with self.subTest(status=status), self.assertRaisesRegex(RuntimeError, 'disabled'):
                ci.audit_tests(self.inventory, junit)

    def test_disabled_inventory_is_rejected_even_with_success_junit(self):
        for value in (True, 'TRUE', 'ON', '1'):
            self.inventory['tests'][-1]['properties'] = [dict(name='DISABLED', value=value)]
            with self.subTest(value=value), self.assertRaisesRegex(RuntimeError, 'Disabled'):
                ci.audit_tests(self.inventory, self.xml())
        for value in (False, 'OFF', '0'):
            self.inventory['tests'][-1]['properties'] = [dict(name='DISABLED', value=value)]
            self.assertEqual(ci.audit_tests(self.inventory, self.xml()), len(self.names))

    def test_failed_and_missing_commands_retain_receipts(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            report = dict(commands=[])
            run = ci.Commands(directory, directory, None, report)
            with self.assertRaisesRegex(RuntimeError, 'failed'):
                run('failure', [sys.executable, '-c', 'print("diagnostic", flush=True); raise SystemExit(7)'], quiet=True)
            receipt = json.loads((directory / 'execution.json').read_text())
            self.assertEqual(receipt['commands'][0]['returncode'], 7)
            self.assertIn('diagnostic', (directory / 'failure.log').read_text())
            with self.assertRaises(OSError):
                run('missing', [str(directory / 'nonexistent-executable')])
            receipt = json.loads((directory / 'execution.json').read_text())
            self.assertIn('error', receipt['commands'][-1])
            self.assertIn('finished', receipt['commands'][-1])

    def test_release_scope_and_python_contamination(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            cache = '\n'.join(name + ':BOOL=OFF' for name in (
                'BUILD_TESTING', 'THUMBGEN_BUILD_PROBES', 'THUMBGEN_BUILD_VIPS', 'THUMBGEN_BUILD_STRESS_FIXTURES'))
            (directory / 'CMakeCache.txt').write_text(cache)
            reply = directory / '.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            expected = {'thumbgen': 'EXECUTABLE', 'thumbgen_core': 'STATIC_LIBRARY',
                        'thumbgen_runtime': 'STATIC_LIBRARY', 'thumbgen_schedule': 'STATIC_LIBRARY'}
            targets = []
            for name, kind in expected.items():
                (reply / (name + '.json')).write_text(json.dumps(dict(name=name, type=kind)))
                targets.append(dict(jsonFile=name + '.json'))
            (reply / 'index-1.json').write_text(json.dumps({'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}}))
            (reply / 'model.json').write_text(json.dumps(dict(configurations=[dict(targets=targets)])))
            self.assertEqual(ci.audit_release(directory), expected)
            (reply / 'model.json').write_text(json.dumps(dict(configurations=[dict(targets=targets + targets[:1])])))
            with self.assertRaisesRegex(RuntimeError, 'scope'):
                ci.audit_release(directory)
            (reply / 'model.json').write_text(json.dumps(dict(configurations=[dict(targets=targets)])))
            for variable in ('_Python3_EXECUTABLE', 'PYTHON_EXECUTABLE'):
                (directory / 'CMakeCache.txt').write_text(cache + '\n' + variable + ':INTERNAL=unexpected')
                with self.subTest(variable=variable), self.assertRaisesRegex(RuntimeError, 'Python'):
                    ci.audit_release(directory)
            (directory / 'CMakeCache.txt').write_text(cache)
            (reply / 'thumbgen.json').write_text(json.dumps(dict(name='thumbgen_test', type='EXECUTABLE')))
            with self.assertRaisesRegex(RuntimeError, 'scope'):
                ci.audit_release(directory)


if __name__ == '__main__':
    unittest.main()
