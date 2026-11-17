"""Focused checks for the release directory contract and Linux metadata."""
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

from repack_runtime import renamed, repack


class ReleaseArchiveTests(unittest.TestCase):
    def payload(self):
        return {'bin/thumbgen': b'program', 'lib/runtime.so.1': b'library',
                'share/ThumbGen/branding/icon.png': b'icon', 'licenses/runtime/LICENSE': b'license',
                **{name: b'metadata' for name in ('LICENSE', 'NOTICE', 'DEPENDENCIES.json', 'README.txt', 'SHA256SUMS')}}

    def test_zip_fixed_root_and_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            source, target = Path(folder) / 'old.zip', Path(folder) / 'new.zip'
            with zipfile.ZipFile(source, 'w') as archive:
                for name, data in self.payload().items():
                    archive.writestr('thumbgen-0.1.1-windows-x64/' + name, data)
            repack(source, target)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(set(archive.namelist()), {'ThumbGen/' + name for name in self.payload()})
                for name, data in self.payload().items():
                    self.assertEqual(archive.read('ThumbGen/' + name), data)

    def test_tar_preserves_executable_and_relative_symlink(self):
        with tempfile.TemporaryDirectory() as folder:
            source, target = Path(folder) / 'old.tar.gz', Path(folder) / 'new.tar.gz'
            with tarfile.open(source, 'w:gz') as archive:
                for name, data in self.payload().items():
                    info = tarfile.TarInfo('version-platform/' + name)
                    info.size, info.mode = len(data), 0o755 if name == 'bin/thumbgen' else 0o644
                    archive.addfile(info, io.BytesIO(data))
                info = tarfile.TarInfo('version-platform/lib/runtime.so')
                info.type, info.linkname = tarfile.SYMTYPE, 'runtime.so.1'
                archive.addfile(info)
            repack(source, target)
            with tarfile.open(target) as archive:
                self.assertEqual(archive.getmember('ThumbGen/bin/thumbgen').mode, 0o755)
                link = archive.getmember('ThumbGen/lib/runtime.so')
                self.assertTrue(link.issym())
                self.assertEqual(link.linkname, 'runtime.so.1')
                for name, data in self.payload().items():
                    self.assertEqual(archive.extractfile('ThumbGen/' + name).read(), data)

    def test_rejects_unsafe_or_multiple_roots(self):
        for names in (['a/../bin/thumbgen'], ['/absolute'], ['a/bin/x', 'b/bin/y']):
            with self.assertRaises(ValueError):
                renamed(names)


if __name__ == '__main__':
    unittest.main()
