"""Prepare isolated Linux product dependencies after the pinned OIIO SDK build.

Does not modify the OIIO SDK, install system packages, or cache installed outputs.
The public builders pin source commits; original logs/receipts survive failure.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def plan(repo, sdk, output, python):
    sdk = sdk.resolve(strict=True)
    output = output.resolve()
    if output.exists():
        raise ValueError('Product dependency output must be fresh')
    if sdk == output or sdk in output.parents or output in sdk.parents:
        raise ValueError('Product dependency output overlaps the read-only SDK')
    for path in (sdk, output):
        if any(c in str(path) for c in (';', ':', '\n', '\r')) and os.name != 'nt':
            raise ValueError('Unsupported path separator in dependency prefix')
    for relative in ('sdk', 'oiio-deps/dist', 'oiio-deps/OpenEXR', 'oiio-deps/libjpeg-turbo'):
        if not (sdk / relative).is_dir():
            raise ValueError('Missing pinned SDK input: ' + relative)
    colour = output / 'colour'
    bounded = output / 'bounded-openexr'
    commands = [
        ('colour', [python, str(repo / 'scripts/build-color-sdk.py'), '--root', str(colour),
                    '--jpeg-cache', str(sdk / 'oiio-deps/libjpeg-turbo')]),
        ('bounded-openexr', [python, str(repo / 'scripts/dependencies/build_bounded_openexr.py'),
                             '--source-cache', str(sdk / 'oiio-deps/OpenEXR'),
                             '--deps-prefix', str(sdk / 'oiio-deps/dist'), '--output', str(bounded)])]
    prefixes = [colour / 'sdk', bounded / 'prefix', sdk / 'sdk', sdk / 'oiio-deps/dist']
    return dict(commands=commands, sdk_prefix=';'.join(map(str, prefixes)),
                runtime_dirs=[str(p / sub) for p in prefixes for sub in ('lib', 'lib64')])


def prepare(repo, sdk, output, python, runner=subprocess.run):
    work = plan(repo, sdk, output, python)
    output.mkdir(parents=True, exist_ok=False)
    inputs = ['dependencies/color-sdk.json', 'dependencies/patches/openexr-bounded-zstd-v1.patch',
              'scripts/build-color-sdk.py', 'scripts/dependencies/build_bounded_openexr.py',
              'scripts/linux/sources-lock.json', 'scripts/ci/product-deps.py']
    report = dict(status='running', commands=[], inputs_sha256={p: digest(repo / p) for p in inputs})
    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    try:
        env = dict(os.environ, GIT_TERMINAL_PROMPT='0')
        for name, command in work['commands']:
            record = dict(name=name, command=command)
            report['commands'].append(record)
            save()
            with (output / (name + '.log')).open('wb') as stream:
                try:
                    result = runner(command, cwd=repo, env=env, stdout=stream, stderr=subprocess.STDOUT)
                    record['returncode'] = result.returncode
                except OSError as error:
                    record['error'] = str(error)
                    raise
            if result.returncode:
                raise RuntimeError(name + ' builder failed: ' + str(result.returncode))
            receipt = output / name / 'receipt.json'
            report[name + '_receipt_sha256'] = digest(receipt)
            save()
        environment = dict(THUMBGEN_SDK_PREFIX=work['sdk_prefix'],
                           LD_LIBRARY_PATH=':'.join(work['runtime_dirs']),
                           THUMBGEN_REQUIRE_BOUNDED_ZSTD='1')
        env_file = output / 'environment.json'
        env_file.write_text(json.dumps(environment, indent=2) + '\n', encoding='utf-8')
        report.update(status='passed', environment_sha256=digest(env_file))
        return environment
    except BaseException as error:
        report.update(status='failed', failure=str(error))
        raise
    finally:
        save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('This entry point prepares the Linux CI source SDK only')
    repo = Path(__file__).resolve().parents[2]
    print(json.dumps(prepare(repo, args.sdk, args.output, sys.executable)))


if __name__ == '__main__':
    main()
