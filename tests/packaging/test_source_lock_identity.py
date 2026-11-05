"""Lock identity survives checkout line endings, never arbitrary JSON rewrites."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('runtime_closure', ROOT / 'scripts/windows/runtime_closure.py')
closure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(closure)


def receipt(raw):
    return dict(kind='thumbgen-windows-source-sdk',
                source_lock_sha256=hashlib.sha256(raw).hexdigest(),
                source_lock=json.loads(raw.decode('utf-8-sig')))


class SourceLockIdentity(unittest.TestCase):
    def setUp(self):
        self.lf = b'{\n  "dependencies": [{"package": "codec", "commit": "1234"}],\n  "version": 1\n}\n'
        self.crlf = self.lf.replace(b'\n', b'\r\n')

    def test_exact_representations(self):
        for raw in (self.lf, self.crlf, self.lf.replace(b'\n', b'\r\n', 1)):
            with self.subTest(raw=raw):
                self.assertEqual(closure.source_lock_identity(receipt(raw), raw)['match'], 'exact')

    def test_lf_crlf_both_directions_keep_original_digest(self):
        for old, current in ((self.lf, self.crlf), (self.crlf, self.lf)):
            with self.subTest(old=old):
                result = closure.source_lock_identity(receipt(old), current)
                self.assertEqual(result['match'], 'lf-crlf-equivalent')
                self.assertEqual(result['sdk_recorded_sha256'], hashlib.sha256(old).hexdigest())
                self.assertEqual(result['current_raw_sha256'], hashlib.sha256(current).hexdigest())
                self.assertEqual(result['lf_sha256'], hashlib.sha256(self.lf).hexdigest())

    def test_same_json_with_other_whitespace_or_order_is_rejected(self):
        data = json.loads(self.lf)
        for raw in (json.dumps(data).encode(), json.dumps(dict(reversed(list(data.items()))), indent=2).encode(),
                    self.lf.replace(b'  ', b'    '), self.lf.replace(b'\n', b'\r'), b'\xef\xbb\xbf' + self.lf):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                closure.source_lock_identity(receipt(self.lf), raw)

    def test_content_change_is_rejected(self):
        for raw in (self.lf.replace(b'1234', b'abcd'), self.lf.replace(b'1\n', b'2\n')):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                closure.source_lock_identity(receipt(self.lf), raw)

    def test_semantics_cannot_replace_unknown_hash(self):
        source = receipt(self.lf)
        source['source_lock_sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            closure.source_lock_identity(source, self.lf)

    def test_receipt_profile_and_json_types_must_match(self):
        source = receipt(self.lf)
        source['kind'] = 'another-sdk'
        with self.assertRaises(ValueError):
            closure.source_lock_identity(source, self.lf)
        source = receipt(self.lf)
        source['source_lock']['version'] = True
        with self.assertRaises(ValueError):
            closure.source_lock_identity(source, self.lf)


if __name__ == '__main__':
    unittest.main()
