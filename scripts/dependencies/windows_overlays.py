"""Read-only, explicitly approved Windows overlays; never a DLL search path.

Receipts are allowlisted by exact digest because the historical color helper did
not record its own identity. A new install requires review and a new pin here;
an arbitrary self-authored installed_sha256 map is deliberately insufficient.
These local provenance checks are not signed supply-chain attestations.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]
PINS = {
    'color': '9566ce83dc9a39ab1ee2afde6bac2cfcbffb77298cddf3333c93ef525beab82b',
    'bounded': '8a476149c8c50d6b6d8f008316ccd8c2bdd2a6647bb6c2e229e0a5b555b961de',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def safe(root, relative):
    # Historical Windows color receipts used backslashes; accept one separator
    # representation but reject aliases, drives, ADS, dot components and links.
    text = relative.replace('\\', '/')
    path = PurePosixPath(text)
    if (not text or ':' in text or path.is_absolute() or
            any(part in ('', '.', '..') or part.endswith((' ', '.')) for part in text.split('/'))):
        raise ValueError('Unsafe overlay path: ' + relative)
    target = root.joinpath(*path.parts)
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError('Overlay path escapes prefix: ' + relative)
    for parent in (target, *target.parents):
        if parent == root:
            break
        if parent.is_symlink() or (hasattr(parent, 'is_junction') and parent.is_junction()):
            raise ValueError('Linked overlay path: ' + relative)
    return target


def installed(prefix, records):
    result = {}
    for relative, checksum in records.items():
        path = safe(prefix, relative)
        key = relative.replace('\\', '/').casefold()
        if key in result:
            raise ValueError('Ambiguous overlay artifact: ' + relative)
        if not re.fullmatch('[0-9a-f]{64}', checksum) or sha(path) != checksum:
            raise ValueError('Overlay artifact digest differs: ' + relative)
        result[key] = path
    actual = {p.relative_to(prefix).as_posix().casefold() for p in prefix.rglob('*') if p.is_file()}
    if actual != set(result):
        raise ValueError('Overlay installation inventory differs')
    return result


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args])


def identity(source, commit):
    if git(source, 'rev-parse', 'HEAD').decode().strip() != commit:
        raise ValueError('Overlay upstream commit differs: ' + str(source))


def recipe(relative, expected):
    path = safe(REPO, relative)
    # Git text checkout policy may translate CRLF. Pin the complete LF text.
    if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != expected:
        raise ValueError('Overlay recipe/lock digest differs: ' + relative)
    return dict(file=relative, lf_sha256=expected, raw_sha256=sha(path))


def verify(kind, prefix):
    prefix = prefix.resolve(strict=True)
    root = prefix.parent
    receipt_file = root / 'receipt.json'
    if sha(receipt_file) != PINS[kind]:
        raise ValueError('Unapproved ' + kind + ' overlay receipt')
    receipt = load(receipt_file)
    files = installed(prefix, receipt['installed_sha256'])
    report = dict(kind=kind, prefix=str(prefix), receipt_sha256=sha(receipt_file),
                  installed_sha256={p.relative_to(prefix).as_posix(): sha(p) for p in files.values()},
                  recipes=[], components=[])
    dlls, notices = {}, {}
    if kind == 'color':
        report['recipes'] = [recipe('dependencies/color-sdk.json', COLOR_LOCK),
                             recipe('scripts/build-color-sdk.py', COLOR_HELPER)]
        lock = load(REPO / 'dependencies/color-sdk.json')
        if {x['package'] for x in receipt['components']} != set(lock):
            raise ValueError('Color component set differs')
        for component in receipt['components']:
            name = component['package']
            expected = lock[name]
            for key in ('repository', 'commit', 'version', 'license', 'license_file', 'options'):
                if component[key] != expected[key]:
                    raise ValueError('Color lock differs: ' + name + '/' + key)
            source = root / (name + '-source')
            identity(source, expected['commit'])
            if git(source, 'diff', 'HEAD', '--'):
                raise ValueError('Color source is modified: ' + name)
            entries = expected.get('notices', [dict(path=expected.get('notice'), sha256=expected.get('notice_sha256'))])
            indexed = []
            for entry in entries:
                original = safe(REPO / 'dependencies', entry['path'])
                filename = 'README.ijg' if entry['path'].endswith('README.ijg') else expected['license_file']
                committed = git(source, 'show', expected['commit'] + ':' + filename)
                if (sha(original) != entry['sha256'] or
                        original.read_bytes().replace(b'\r\n', b'\n') != committed.replace(b'\r\n', b'\n')):
                    raise ValueError('Color notice differs: ' + entry['path'])
                destination = 'licenses/overlay-' + name + '/' + filename
                notices[destination] = original.read_bytes()
                indexed.append(dict(file=destination, sha256=entry['sha256']))
            dll = files['bin/' + ('lcms2.dll' if name == 'LittleCMS2' else 'jpeg8.dll')]
            package = 'overlay-' + name
            dlls[dll.name.casefold()] = (dll, dict(name=package, version=expected['version'],
                source_commit=expected['commit'], url=expected['repository']), dict(sha256=sha(dll)))
            report['components'].append(dict(expected, name=package, notices=indexed, linkage='shared'))
    else:
        report['recipes'] = [recipe('scripts/dependencies/build_bounded_openexr.py', BOUNDED_HELPER_LF),
                             recipe('dependencies/patches/openexr-bounded-zstd-v1.patch', PATCH)]
        if (receipt['upstream'] != UPSTREAM or receipt['helper_sha256'] != BOUNDED_HELPER or
                receipt['patch_sha256'] != PATCH or receipt['runtime_identity'] != '-dev-thumbgen-bounded-zstd-v1'):
            raise ValueError('Bounded Core identity differs')
        source = root / 'source'
        identity(source, UPSTREAM)
        patch = REPO / 'dependencies/patches/openexr-bounded-zstd-v1.patch'
        subprocess.run(['git', '-C', str(source), 'apply', '--reverse', '--check', str(patch)], check=True, capture_output=True)
        changed = set(git(source, 'diff', '--name-only', 'HEAD').decode().splitlines())
        if changed != set(receipt['source_sha256']):
            raise ValueError('Unexpected bounded source changes')
        for relative, checksum in receipt['source_sha256'].items():
            if sha(safe(source, relative)) != checksum:
                raise ValueError('Patched source differs: ' + relative)
        indexed = []
        for relative in git(source, 'ls-files', '-z').decode().split('\0'):
            if not relative or not PurePosixPath(relative).name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'PATENTS')):
                continue
            data = git(source, 'show', UPSTREAM + ':' + relative)
            if safe(source, relative).read_bytes().replace(b'\r\n', b'\n') != data.replace(b'\r\n', b'\n'):
                raise ValueError('Bounded notice differs: ' + relative)
            destination = 'licenses/overlay-OpenEXRCore/' + relative
            notices[destination] = data
            indexed.append(dict(file=destination, sha256=hashlib.sha256(data).hexdigest()))
        if not indexed:
            raise ValueError('Missing bounded Core notices')
        library = files['lib/openexrcore_v_3_5_0_openimageio_v3_2.lib']
        report['static_library'] = dict(path=str(library), sha256=sha(library))
        report['components'].append(dict(name='overlay-OpenEXRCore', version='3.5.0-dev', commit=UPSTREAM,
            repository='https://github.com/AcademySoftwareFoundation/openexr', patch_sha256=PATCH,
            linkage='static; separate from OIIO embedded original OpenEXR', notices=indexed))
        notices['provenance/patches/openexr-bounded-zstd-v1.patch'] = patch.read_bytes()
    return report, dlls, notices


# Approved build receipts above bind all installed binaries. Recipe pins bind the
# reviewed construction independently of a caller's receipt or directory name.
COLOR_LOCK = '795f1bd5040557f5715dd0ab694b8fe2d84ce3cd290b87250877741351c84dda'
COLOR_HELPER = '6913e76c3bf1a4139b14e479b3efb9a6e4fb0276562ed8ab6ebbea318c3a0c88'
BOUNDED_HELPER = 'bb8cd9c12e2b125226ac4bfcee62eca38da7156742b82d3eba3110bce5272258'
BOUNDED_HELPER_LF = '6782100cfe3a07176b488cc4401c1f63f46f36e1c3282553ee397c4cefcbe732'
PATCH = '46738fa7c6cdee48511936e847f02079a8da370dc3bcfcc95be5ea863a416a45'
UPSTREAM = '5d838e01770cadb8f2a36c0d17018452fa7a3c0c'
