"""Ensure public smoke rejects successful-looking incomplete protocol output."""
import copy
import importlib.util
from pathlib import Path
import unittest
import tempfile

spec = importlib.util.spec_from_file_location('smoke', Path(__file__).resolve().parents[2] / 'scripts/ci/smoke_examples.py')
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class Validator:
    def validate(self, row):
        pass  # Isolate arithmetic checks; real runs also apply the public schema.


class ExampleSmoke(unittest.TestCase):
    def test_new_outputs_are_separate_from_protected_inputs(self):
        with tempfile.TemporaryDirectory() as root:
            deps = Path(root)
            inputs = deps / 'public-inputs'
            inputs.mkdir()
            path = inputs / 'fixture.png'
            path.write_bytes(b'input')
            before = smoke.snapshot_inputs(deps)
            outputs = deps / 'public-output'
            outputs.mkdir()
            (outputs / 'preview.png').write_bytes(b'output')
            self.assertEqual(smoke.snapshot_inputs(deps), before)
            path.write_bytes(b'changed')
            self.assertNotEqual(smoke.snapshot_inputs(deps), before)
            path.write_bytes(b'input')
            (inputs / 'unexpected.png').write_bytes(b'added')
            self.assertNotEqual(smoke.snapshot_inputs(deps), before)

    def setUp(self):
        self.manifest = dict(jobs=[dict(id='first', outputs=[dict(path='out.png')])])
        self.rows = [dict(type='result', id='first', ok=True, status='success', outputs=[dict(status='success')]),
                     dict(type='summary', total=1, success=1, failed=0, partial=0, skipped=0, cancelled=0,
                          output_counts=dict(total=1, success=1, failed=0, skipped=0, cancelled=0, not_attempted=0))]

    def test_complete_result(self):
        self.assertEqual(len(smoke.audit_rows(self.manifest, self.rows, Validator())), 1)

    def test_duplicate_missing_and_unknown_ids(self):
        for rows in ([self.rows[1]], [self.rows[0], *self.rows],
                     [dict(self.rows[0], id='unknown'), self.rows[1]]):
            with self.subTest(rows=rows), self.assertRaisesRegex(RuntimeError, 'IDs'):
                smoke.audit_rows(self.manifest, rows, Validator())

    def test_summary_must_match_outputs_and_jobs(self):
        for key in ('total', 'success', 'failed', 'skipped', 'cancelled', 'partial'):
            rows = copy.deepcopy(self.rows)
            rows[-1][key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, 'arithmetic'):
                smoke.audit_rows(self.manifest, rows, Validator())
        for key in self.rows[-1]['output_counts']:
            rows = copy.deepcopy(self.rows)
            rows[-1]['output_counts'][key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, 'arithmetic'):
                smoke.audit_rows(self.manifest, rows, Validator())

    def test_skipped_output_and_missing_output_rejected(self):
        for outputs in ([], [dict(status='skipped')]):
            rows = copy.deepcopy(self.rows)
            rows[0]['outputs'] = outputs
            with self.subTest(outputs=outputs), self.assertRaisesRegex(RuntimeError, 'outputs|skipped'):
                smoke.audit_rows(self.manifest, rows, Validator())

    def test_summary_is_final_and_unique(self):
        for rows in (list(reversed(self.rows)), self.rows + [self.rows[-1]]):
            with self.assertRaisesRegex(RuntimeError, 'summary'):
                smoke.audit_rows(self.manifest, rows, Validator())
