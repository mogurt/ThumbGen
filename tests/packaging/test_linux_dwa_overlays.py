"""Portable-DWA selection checks; native byte/alias graphs have separate real tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('dwa_overlays', ROOT / 'scripts/linux/runtime_overlays.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class DwaSelection(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.prefix = self.root / 'prefix'
        self.receipt = dict(installed_sha256={},
            helper_sha256=m.sha(ROOT / 'scripts/dependencies/build_portable_dwa_sdk.py'),
            patch_sha256=m.sha(ROOT / 'dependencies/patches/openexr-portable-dwa-v1.patch'),
            inputs=dict(lock=json.loads((ROOT / 'scripts/linux/sources-lock.json').read_text())))
        (self.root / 'receipt.json').write_text(json.dumps(self.receipt))
        self.products = {}
        for name in ('OpenEXR', 'OpenEXRCore', 'OpenEXRUtil', 'Iex', 'IlmThread'):
            rel = 'lib/lib' + name + '_v_3_5_0_OpenImageIO_v3_2.a'
            self.products[rel] = dict(path=str(self.prefix / rel), native=dict(
                install_rule=dict(kind='STATIC_LIBRARY'), link_inputs={}))
        archives = {p['path']: 'native-hash' for p in self.products.values()
                    if 'OpenEXRUtil' not in p['path']}
        for name in ('OpenImageIO', 'OpenImageIO_Util'):
            rel = 'lib/lib' + name + '.so.3.2.1'
            self.products[rel] = dict(path=str(self.prefix / rel), native=dict(
                install_rule=dict(kind='SHARED_LIBRARY'), link_inputs=dict(archives) if name == 'OpenImageIO' else {}))
        self.shared = [p['path'] for p in self.products.values() if '.so.' in p['path']]
        self.installation = dict(products=self.products, tools={}, aliases={},
            verified_files={p['path']: 'native-hash' for p in self.products.values()})
        self.base = dict(products={}, tools={}, verified_files={})
        self.patches = [patch.object(m, 'locked_tree', return_value='tree'),
            patch.object(m, 'build_evidence', return_value={}),
            patch.object(m, 'audit_installations', return_value=self.installation),
            patch.object(m, 'check_native_link_inputs'),
            patch.object(m, 'cache_values', return_value=dict(OpenEXR_DIR=str(self.prefix / 'lib/cmake/OpenEXR'),
                OpenImageIO_DIR=str(self.prefix / 'lib/cmake/OpenImageIO'))),
            patch.object(m._native, 'query', return_value=('LINKER', self.shared, 'query')),
            patch.object(m.subprocess, 'check_output', return_value='\n'.join(self.shared)),
            patch.object(m.re, 'findall', side_effect=lambda pattern, text: text.splitlines())]
        self.mocks = [p.start() for p in self.patches]
        for p in self.patches: self.addCleanup(p.stop)

    def audit(self):
        return m.audit_dwa(ROOT, self.root, self.base, self.root / 'product')

    def test_selected_native_products_only(self):
        result = self.audit()
        self.assertNotIn('forged.bin', result['installation']['verified_files'])
        self.assertEqual(len(result['static_embedding']), 4)
        self.assertEqual(len(result['installed_static_not_embedded']), 1)
        self.assertEqual(self.mocks[0].call_count, 2)
        self.assertEqual(self.mocks[0].call_args_list[0].args[2].name, 'openexr-portable-dwa-v1.patch')
        self.assertEqual(self.mocks[0].call_args_list[1].args[2], None)
        self.mocks[3].assert_called_once()

    def test_old_flat_core_link_refused(self):
        main = self.products['lib/libOpenImageIO.so.3.2.1']['native']['link_inputs']
        main.pop(next(k for k in main if 'OpenEXRCore' in k))
        with self.assertRaisesRegex(ValueError, 'does not embed'):
            self.audit()

    def test_other_product_oiio_config_refused(self):
        self.mocks[4].return_value['OpenImageIO_DIR'] = str(self.root / 'old-sdk')
        with self.assertRaisesRegex(ValueError, 'another OpenImageIO'):
            self.audit()

    def test_other_flat_openexr_config_refused(self):
        self.mocks[4].return_value['OpenEXR_DIR'] = str(self.root / 'bounded-core')
        with self.assertRaisesRegex(ValueError, 'another OpenEXR'):
            self.audit()

    def test_old_product_link_refused(self):
        self.mocks[5].return_value = ('LINKER', ['/old/libOpenImageIO.so.3.2.1'], 'query')
        with self.assertRaisesRegex(ValueError, 'does not link'):
            self.audit()

    def test_loader_old_util_refused(self):
        self.mocks[6].return_value = self.shared[0] + '\n/old/libOpenImageIO_Util.so.3.2.1'
        with self.assertRaisesRegex(ValueError, 'loader does not select'):
            self.audit()

    def test_receipt_cannot_authorize_unknown_payload(self):
        self.receipt['installed_sha256'] = {'unknown': 'summary'}
        (self.root / 'receipt.json').write_text(json.dumps(self.receipt))
        with self.assertRaisesRegex(ValueError, 'DWA installation changed'):
            self.audit()

    def test_unknown_builder_refused(self):
        self.receipt['helper_sha256'] = '0' * 64
        (self.root / 'receipt.json').write_text(json.dumps(self.receipt))
        with self.assertRaisesRegex(ValueError, 'public builder'):
            self.audit()

    def test_verified_dependencies_required(self):
        # This fails before any receipt can supply external link authority.
        with self.assertRaisesRegex(ValueError, 'verified base dependencies'):
            m.audit_dwa(ROOT, self.root, None, self.root / 'product')


if __name__ == '__main__':
    unittest.main()
