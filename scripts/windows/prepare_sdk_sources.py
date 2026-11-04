"""Prepare independent pinned Windows SDK sources, optionally from local Git objects."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from sdk_patches import approved_changes

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache', type=Path, required=True)
parser.add_argument('--source-cache', type=Path, help='Optional dependency Git repositories by package name')
parser.add_argument('--oiio-source', type=Path, help='Optional existing pinned OIIO Git repository')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
lock = json.loads((repo / 'dependencies/windows-source-sdk.json').read_text(encoding='utf-8-sig'))
environment = dict(os.environ, GIT_TERMINAL_PROMPT='0', GIT_OPTIONAL_LOCKS='0')
items = [dict(package='OpenImageIO', commit=lock['oiio_commit'],
              repository='https://github.com/AcademySoftwareFoundation/OpenImageIO.git',
              destination='oiio-source', local=args.oiio_source)]
items += [dict(item, destination='oiio-deps/' + item['package'],
               local=args.source_cache / item['package'] if args.source_cache else None)
          for item in lock['dependencies']]
items += [dict(lock['nlohmann_json'], package='nlohmann_json', repository='https://github.com/nlohmann/json.git',
               destination='json-source', local=None)]
receipt = []
for item in items:
    destination = args.cache.resolve() / item['destination']
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        if item['local']:
            local = str(item['local'])
            actual = subprocess.check_output(['git', '-c', 'safe.directory=' + local, '-C', local,
                                              'rev-parse', 'HEAD'], text=True, env=environment).strip()
            if actual != item['commit']:
                raise RuntimeError('Local source commit differs: ' + item['package'])
            subprocess.run(['git', '-c', 'safe.directory=' + local, '-c', 'core.autocrlf=false',
                            'clone', '--no-hardlinks', '--local', '--no-checkout', local, str(destination)],
                           check=True, env=environment)
        else:
            subprocess.run(['git', 'init', str(destination)], check=True, env=environment)
            subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', item['repository']],
                           check=True, env=environment)
            subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', item['commit']],
                           check=True, env=environment)
        subprocess.run(['git', '-C', str(destination), '-c', 'core.autocrlf=false',
                        'checkout', '--detach', item['commit']], check=True, env=environment)
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'],
                                     text=True, env=environment).strip()
    if actual != item['commit'] or (destination / '.git/objects/info/alternates').exists():
        raise RuntimeError('Independent source identity differs: ' + item['package'])
    # OIIO helpers check out and verify upstream tag names against fixed commits.
    if item['package'] not in ('OpenImageIO', 'nlohmann_json'):
        # Local clones retain upstream tags. Network preparation needs the tag
        # objects too; moved tags still fail the helpers' fixed commit check.
        if not item['local']:
            subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', '--tags', 'origin'],
                           check=True, env=environment)
    changed = subprocess.check_output(['git', '-C', str(destination), '-c', 'core.autocrlf=false',
                                       'status', '--porcelain'], text=True, env=environment)
    generated_zlib_header = False
    if item['package'] == 'ZLIB' and changed.strip() == 'D zconf.h':
        # Zlib's pinned CMakeLists moves this tracked source header aside so
        # its build-generated header takes precedence. Accept only that exact
        # generated rename, with the original bytes preserved.
        original = subprocess.check_output(['git', '-C', str(destination), 'show', 'HEAD:zconf.h'], env=environment)
        renamed = destination / 'zconf.h.included'
        generated_zlib_header = renamed.is_file() and renamed.read_bytes() == original
    approved_oiio_patch = item['package'] == 'OpenImageIO' and approved_changes(destination, changed)
    if changed and not generated_zlib_header and not approved_oiio_patch:
        raise RuntimeError('Pinned source working tree has changes: ' + item['package'])
    receipt.append(dict(package=item['package'], commit=actual, repository=item['repository'],
                        destination=item['destination'], local_objects_reused=bool(item['local'])))
(args.cache / 'source-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(pinned_sources=len(receipt))))
