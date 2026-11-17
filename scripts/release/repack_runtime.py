"""Give a prepared runtime archive the fixed ThumbGen/ installation directory.

The input is an already assembled, audited single-root ZIP or tar.gz. Payload
bytes, tar symlinks and Unix modes are preserved; only member names change.
Usage: python scripts/release/repack_runtime.py INPUT OUTPUT
Extract OUTPUT into the parent of the desired ThumbGen installation directory.
"""
import argparse
import copy
import gzip
from pathlib import Path, PurePosixPath
import posixpath
import tarfile
import zipfile


def renamed(names):
    roots = set()
    result = {}
    for name in names:
        path = PurePosixPath(name)
        if (path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name
                or not path.parts or name.rstrip('/') != path.as_posix()):
            raise ValueError('Unsafe archive member: ' + name)
        roots.add(path.parts[0])
        result[name] = '/'.join(('ThumbGen', *path.parts[1:])) + ('/' if name.endswith('/') else '')
    if len(roots) != 1 or len(set(result.values())) != len(result):
        raise ValueError('Expected one unique enclosing directory')
    paths = set(result.values())
    for required in ('DEPENDENCIES.json', 'LICENSE', 'NOTICE', 'README.txt', 'SHA256SUMS'):
        if 'ThumbGen/' + required not in paths:
            raise ValueError('Missing runtime metadata: ' + required)
    for directory in ('bin/', 'share/', 'licenses/'):
        if not any(p.startswith('ThumbGen/' + directory) for p in paths):
            raise ValueError('Missing runtime directory: ' + directory)
    if not paths.intersection(('ThumbGen/bin/thumbgen', 'ThumbGen/bin/thumbgen.exe')):
        raise ValueError('Missing runtime executable')
    return result


def repack(source, destination):
    source, destination = Path(source), Path(destination)
    if destination.exists():
        raise ValueError('Destination already exists')
    if source.name.endswith('.zip') and destination.name.endswith('.zip'):
        with zipfile.ZipFile(source) as original:
            names = renamed(original.namelist())
            with zipfile.ZipFile(destination, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as output:
                for entry in original.infolist():
                    info = copy.copy(entry)
                    info.filename = names[entry.filename]
                    output.writestr(info, original.read(entry))
    elif source.name.endswith('.tar.gz') and destination.name.endswith('.tar.gz'):
        with tarfile.open(source, 'r:gz') as original:
            entries = original.getmembers()
            names = renamed([m.name for m in entries])
            for entry in entries:
                if not (entry.isfile() or entry.isdir() or entry.issym()):
                    raise ValueError('Unsupported tar member: ' + entry.name)
                if entry.issym():
                    target = posixpath.normpath(posixpath.join(posixpath.dirname(names[entry.name]), entry.linkname))
                    if entry.linkname.startswith('/') or '\\' in entry.linkname or not target.startswith('ThumbGen/'):
                        raise ValueError('Symlink escapes installation: ' + entry.name)
            with destination.open('xb') as stream, gzip.GzipFile(filename='', fileobj=stream, mode='wb', mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as output:
                    for entry in entries:
                        info = copy.copy(entry)
                        info.name = names[entry.name]
                        info.pax_headers = {k: v for k, v in entry.pax_headers.items() if k != 'path'}
                        output.addfile(info, original.extractfile(entry) if entry.isfile() else None)
    else:
        raise ValueError('Input/output must both be ZIP or both tar.gz')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    repack(args.input, args.output)
