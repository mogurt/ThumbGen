"""Fetch public locked sources into a NEW CI seed, never a private SDK cache."""
import json
import os
from pathlib import Path
import subprocess


def main():
    repo = Path(__file__).resolve().parents[2]
    seed = repo / '.deps/ci-linux-seed'
    seed.mkdir(parents=True, exist_ok=False)
    lock = json.loads((repo / 'scripts/linux/sources-lock.json').read_text())
    items = [dict(package='OpenImageIO', commit=lock['oiio_commit'],
                  repository='https://github.com/AcademySoftwareFoundation/OpenImageIO.git',
                  destination='oiio-source')]
    items += [dict(item, destination='oiio-deps/' + item['package']) for item in lock['dependencies']]
    items += [dict(lock['nlohmann_json'], package='nlohmann_json',
                   repository='https://github.com/nlohmann/json.git', destination='src/json')]
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0')
    records = []
    for item in items:
        destination = seed / item['destination']
        destination.parent.mkdir(parents=True, exist_ok=True)
        def git(*args):
            return subprocess.check_output(['git', '-c', 'core.hooksPath=/dev/null',
                                            '-C', str(destination), *args], env=env, text=True)
        subprocess.run(['git', 'init', str(destination)], env=env, check=True)
        git('remote', 'add', 'origin', item['repository'])
        git('fetch', '--depth=1', 'origin', item['commit'])
        # OIIO's dependency builders verify tag -> fixed commit before building.
        if item['package'] not in ('OpenImageIO', 'nlohmann_json'):
            git('fetch', '--depth=1', '--tags', 'origin')
        git('checkout', '--detach', item['commit'])
        git('fsck', '--full')
        actual = git('rev-parse', 'HEAD').strip()
        if actual != item['commit'] or git('status', '--porcelain').strip():
            raise RuntimeError('Pinned source identity differs: ' + item['package'])
        records.append(dict(package=item['package'], commit=actual, repository=item['repository']))
        (seed / 'public-sources.json').write_text(json.dumps(records, indent=2) + '\n')
    # Existing SDK entry point expects a tools environment; use only host tools.
    (seed / 'environment.sh').write_text('# CI uses the explicitly provisioned host toolchain.\n')
    print(json.dumps(dict(public_sources=len(records), seed=str(seed))))


if __name__ == '__main__':
    main()
