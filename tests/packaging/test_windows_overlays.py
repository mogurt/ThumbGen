"""Overlay trust boundaries without requiring a Windows SDK in unit tests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import shutil
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('overlays', ROOT / 'scripts/dependencies/windows_overlays.py')
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)
spec = importlib.util.spec_from_file_location('closure', ROOT / 'scripts/windows/runtime_closure.py')
closure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(closure)


class OverlayBoundaries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_safe_historical_windows_separator(self):
        self.assertEqual(overlay.safe(self.root, 'bin\\jpeg8.dll'), self.root / 'bin/jpeg8.dll')

    def test_unsafe_relative_paths(self):
        for value in ('/bin/a.dll', 'C:/a.dll', 'C:a.dll', '../a', 'a/../b', 'a//b',
                      './a', '\\\\server\\share', 'a:stream', 'a./b', 'a /b', ''):
            with self.subTest(value=value), self.assertRaises(ValueError):
                overlay.safe(self.root, value)

    def test_inventory_success_and_tampering(self):
        file = self.root / 'a.dll'
        file.write_bytes(b'approved')
        records = {'a.dll': overlay.sha(file)}
        self.assertEqual(overlay.installed(self.root, records), {'a.dll': file})
        file.write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'digest differs'):
            overlay.installed(self.root, records)

    def test_extra_unrecorded_artifact(self):
        (self.root / 'injected.dll').write_bytes(b'injected')
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            overlay.installed(self.root, {})

    def test_case_or_separator_alias_is_rejected(self):
        (self.root / 'bin').mkdir()
        file = self.root / 'bin/a.dll'
        file.write_bytes(b'a')
        for alias in ('bin\\a.dll', 'BIN/A.DLL'):
            with self.subTest(alias=alias), self.assertRaises(ValueError):
                overlay.installed(self.root, {'bin/a.dll': overlay.sha(file), alias: overlay.sha(file)})

    def test_duplicate_json_keys(self):
        file = self.root / 'receipt.json'
        file.write_text('{"installed_sha256": {}, "installed_sha256": {}}')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON'):
            overlay.load(file)

    def test_self_authored_generic_receipt_is_not_approved(self):
        prefix = self.root / 'sdk'
        prefix.mkdir()
        (self.root / 'receipt.json').write_text(json.dumps(dict(components=[], installed_sha256={})))
        with self.assertRaisesRegex(ValueError, 'Unapproved color'):
            overlay.verify('color', prefix)

    def test_missing_overlay_fails(self):
        with self.assertRaises(OSError):
            overlay.verify('color', self.root / 'missing')

    def test_recipe_tampering_fails(self):
        with self.assertRaisesRegex(ValueError, 'recipe/lock digest'):
            overlay.recipe('dependencies/color-sdk.json', '0' * 64)

    def test_color_notices_and_component_lock_are_mandatory(self):
        prefix = self.root / 'sdk'
        (prefix / 'bin').mkdir(parents=True)
        (prefix / 'bin/lcms2.dll').write_bytes(b'lcms')
        (prefix / 'bin/jpeg8.dll').write_bytes(b'jpeg')
        dependencies = self.root / 'repository/dependencies'
        shutil.copytree(ROOT / 'dependencies', dependencies)
        lock = overlay.load(dependencies / 'color-sdk.json')
        receipt = dict(components=[dict(package=name, **item) for name, item in lock.items()],
                       installed_sha256={p.relative_to(prefix).as_posix(): overlay.sha(p) for p in prefix.rglob('*') if p.is_file()})
        file = self.root / 'receipt.json'
        file.write_text(json.dumps(receipt))
        def source_git(source, *args):
            if args[0] == 'diff':
                return b''
            name = 'LittleCMS2-2.20.txt' if source.name.startswith('LittleCMS2') else 'libjpeg-turbo-3.1.2-' + args[1].split(':')[-1]
            return (ROOT / 'dependencies/notices' / name).read_bytes()
        with patch.object(overlay, 'REPO', dependencies.parent), \
                patch.dict(overlay.PINS, color=overlay.sha(file)), \
                patch.object(overlay, 'recipe', return_value={}), \
                patch.object(overlay, 'identity'), patch.object(overlay, 'git', side_effect=source_git):
            result, dlls, materials = overlay.verify('color', prefix)
            self.assertEqual(set(dlls), {'lcms2.dll', 'jpeg8.dll'})
            self.assertEqual(len(materials), 3)
            notice = dependencies / 'notices/LittleCMS2-2.20.txt'
            notice.write_bytes(b'changed notice')
            with self.assertRaisesRegex(ValueError, 'notice differs'):
                overlay.verify('color', prefix)
            notice.unlink()
            with self.assertRaises(OSError):
                overlay.verify('color', prefix)
            shutil.copyfile(ROOT / 'dependencies/notices/LittleCMS2-2.20.txt', notice)
            receipt['components'][0]['commit'] = '0' * 40
            file.write_text(json.dumps(receipt))
            with patch.dict(overlay.PINS, color=overlay.sha(file)), self.assertRaisesRegex(ValueError, 'Color lock differs'):
                overlay.verify('color', prefix)

    def test_sdk_overlay_same_name_different_bytes_fails(self):
        sdk = self.root / 'sdk'
        (sdk / 'Library/bin').mkdir(parents=True)
        (sdk / 'Library/bin/jpeg8.dll').write_bytes(b'old')
        replacement = self.root / 'jpeg8.dll'
        replacement.write_bytes(b'new')
        with patch.object(closure, 'conda_metadata', return_value=({}, {}, {'lock_sha256': 'x'})), \
                patch.object(closure.overlays, 'verify', return_value=({}, {'jpeg8.dll':
                    (replacement, {'name': 'overlay-JPEG'}, {})}, {})), \
                self.assertRaisesRegex(ValueError, 'Ambiguous SDK/overlay DLL'):
            closure.audit(self.root / 'thumbgen.exe', sdk, self.root, self.root)

    def test_unknown_dll_has_no_path_or_current_directory_fallback(self):
        exe = self.root / 'thumbgen.exe'
        exe.write_bytes(b'unit-test')
        (self.root / 'unknown.dll').write_bytes(b'injected')
        with patch.object(closure, 'imports', return_value=[{'name': 'unknown.dll', 'kind': 'import'}]), \
                self.assertRaisesRegex(ValueError, 'Unresolved DLL'):
            closure.resolve(exe, self.root, self.root / 'system', {}, {}, {}, set())


if __name__ == '__main__':
    unittest.main()
