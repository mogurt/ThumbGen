"""Build a fresh, auditable Windows overlay using this checkout's trusted helper.

The output directory must not exist. No per-machine receipt allowlist or source
edit is needed. Keep the complete output (sources, build tree, logs and prefix)
until freezing/packaging; deleting native records deliberately fails the audit.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import windows_overlays as policy
import overlay_provenance as provenance


def build(kind, root, lcms_cache=None, jpeg_cache=None, source_cache=None, sdk=None):
    if os.name != 'nt':
        raise ValueError('This native tracking profile requires Windows MSVC; Linux has a separate SDK audit')
    root = root.resolve()
    if root.exists():
        raise ValueError('Output must be a new private directory; prior failures are never overwritten')
    for path in (lcms_cache, jpeg_cache, source_cache, sdk, policy.REPO):
        if path and (path.resolve() == root or root in path.resolve().parents):
            raise ValueError('Output must not contain a read-only input')
    provenance.trusted_recipes(policy, kind)
    helper = policy.REPO / ('scripts/build-color-sdk.py' if kind == 'color' else 'scripts/dependencies/build_bounded_openexr.py')
    prefix = root / ('sdk' if kind == 'color' else 'prefix')
    arguments = [sys.executable, str(helper)]
    if kind == 'color':
        arguments += ['--root', str(root)]
        for key, path in (('--lcms-cache', lcms_cache), ('--jpeg-cache', jpeg_cache)):
            if path:
                arguments += [key, str(path.resolve(strict=True))]
    else:
        if not source_cache or not sdk:
            raise ValueError('Bounded overlay requires --source-cache and --sdk')
        # The old SDK is read only. Validate its fixed source lock and every
        # installed artifact before using its CMake packages as build inputs.
        source = policy.load(sdk / 'sdk/thumbgen-sdk.json')
        expected = policy.load(policy.REPO / 'dependencies/windows-source-sdk.json')
        if source.get('kind') != 'thumbgen-windows-source-sdk' or source['source_lock'] != expected:
            raise ValueError('Bounded build dependency SDK lock differs')
        for relative, entry in source['artifacts'].items():
            if policy.sha(policy.safe(sdk, relative)) != entry['sha256']:
                raise ValueError('Bounded build dependency SDK artifact differs: ' + relative)
        arguments += ['--output', str(root), '--source-cache', str(source_cache.resolve(strict=True)),
                      '--deps-prefix', str(sdk.resolve() / 'oiio-deps/dist')]
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'TEMP', 'TMP', 'COMSPEC', 'SYSTEMDRIVE', 'PROGRAMFILES',
               'PROGRAMFILES(X86)', 'PROGRAMW6432', 'APPDATA', 'LOCALAPPDATA', 'USERPROFILE', 'NUMBER_OF_PROCESSORS'}
    environment = {k: v for k, v in os.environ.items() if k.upper() in allowed}
    environment.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='NUL')
    commands = []
    def run(name, command):
        executable = Path(shutil.which(str(command[0])) or str(command[0])).resolve(strict=True)
        result = subprocess.run(list(map(str, command)), env=environment, capture_output=True)
        root.mkdir(parents=True, exist_ok=True)  # Retain even an early helper failure.
        for suffix, data in (('stdout', result.stdout), ('stderr', result.stderr)):
            with (root / (name + '.' + suffix + '.log')).open('xb') as stream:
                stream.write(data)
        commands.append(dict(name=name, args=list(map(str, command)), executable=str(executable),
                             executable_sha256=policy.sha(executable), exit=result.returncode))
        (root / 'build-commands.json').write_text(json.dumps(commands, indent=2) + '\n', encoding='utf-8')
        if result.returncode:
            raise RuntimeError(name + ' failed; original exit/logs retained in ' + str(root))
    run('helper', arguments)
    names = list(policy.load(policy.REPO / 'dependencies/color-sdk.json')) if kind == 'color' else ['bounded']
    for name in names:
        build_dir = root / (name + '-build') if kind == 'color' else root / 'build'
        run(name + '-install', ['cmake', '-DCMAKE_INSTALL_CONFIG_NAME=Release', '--trace-expand',
            '--trace-format=json-v1', '--trace-redirect=' + str(root / (name + '-install-trace.jsonl')),
            '-P', str(build_dir / 'cmake_install.cmake')])
    observed = provenance.observe(policy, kind, prefix)
    with (root / 'overlay-provenance.json').open('x', encoding='utf-8') as stream:
        json.dump(observed, stream, indent=2); stream.write('\n')
    policy.verify(kind, prefix)
    return dict(prefix=str(prefix), provenance=str(root / 'overlay-provenance.json'),
                sha256=policy.sha(root / 'overlay-provenance.json'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=('color', 'bounded'), required=True)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('lcms-cache', 'jpeg-cache', 'source-cache', 'sdk'):
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.kind, args.root, args.lcms_cache, args.jpeg_cache, args.source_cache, args.sdk)))
