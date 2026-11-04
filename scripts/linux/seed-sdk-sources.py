"""Copy pinned Git source objects into independent clones, never installed artifacts."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('cache', type=Path)
parser.add_argument('source_cache', type=Path)
parser.add_argument('oiio_source', type=Path)
parser.add_argument('lock', type=Path)
args = parser.parse_args()
lock = json.loads(args.lock.read_text())
environment = dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_TERMINAL_PROMPT='0')
sources = [('OpenImageIO', args.oiio_source, args.cache / 'oiio-source', lock['oiio_commit'])]
sources += [(item['package'], args.source_cache / 'oiio-deps' / item['package'],
             args.cache / 'oiio-deps' / item['package'], item['commit']) for item in lock['dependencies']]
sources += [('nlohmann_json', args.source_cache / 'src/json', args.cache / 'src/json', lock['nlohmann_json']['commit'])]
records = []
for name, old, new, commit in sources:
    actual = subprocess.check_output(['git', '-c', f'safe.directory={old}', '-C', str(old), 'rev-parse', 'HEAD'],
                                     env=environment, text=True).strip()
    if actual != commit or new.exists():
        raise RuntimeError('Unexpected source identity or occupied destination: ' + name)
    new.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', '-c', f'safe.directory={old}', 'clone', '--no-hardlinks', '--local', '--no-checkout',
                    str(old), str(new)], env=environment, check=True)
    subprocess.run(['git', '-C', str(new), 'checkout', '--detach', commit], env=environment, check=True)
    if (new / '.git/objects/info/alternates').exists():
        raise RuntimeError('Source clone unexpectedly shares an object database')
    records.append(dict(package=name, source=str(old), destination=str(new), commit=commit,
                        method='independent local Git clone --no-hardlinks; pinned checkout'))
(args.cache / 'source-seed.json').write_text(json.dumps(records, indent=2) + '\n')
