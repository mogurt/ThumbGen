"""Create a deterministic INTERNAL Windows ZIP from a tested minimal SDK build."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
import zipfile

from runtime_closure import audit, digest


def encode(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + '\n').encode('utf-8')


def pack(exe, sdk, evidence_file, output, runtime_notices):
    repo = Path(__file__).resolve().parents[2]
    evidence = json.loads(evidence_file.read_text(encoding='utf-8'))
    sdk_receipt_file = sdk / 'sdk/thumbgen-sdk.json'
    sdk_receipt = json.loads(sdk_receipt_file.read_text(encoding='utf-8'))
    versions = {item['package']: item['version'] for item in sdk_receipt['source_lock']['dependencies']}
    versions.update(OpenImageIO=sdk_receipt['source_lock']['oiio_tag'].lstrip('v'),
                    nlohmann_json=sdk_receipt['source_lock']['nlohmann_json']['version'])
    if (evidence.get('kind') != 'thumbgen-tested-windows-product' or evidence['exe_sha256'] != digest(exe)
            or evidence['sdk_receipt_sha256'] != digest(sdk_receipt_file)):
        raise ValueError('Executable/SDK differs from frozen validation evidence')
    closure = audit(exe, sdk, Path(os.environ['SystemRoot']) / 'System32')
    if closure['files'] != evidence['closure']['files'] or closure['sdk_profile'] != 'source':
        raise ValueError('Runtime closure differs from frozen validation evidence')
    payload, components = {}, []

    def add(relative, value):
        path = PurePosixPath(relative)
        if path.is_absolute() or '..' in path.parts or '\\' in relative or relative in payload:
            raise ValueError('Unsafe/duplicate archive entry: ' + relative)
        payload[relative] = value

    for item in closure['files']:
        original = exe if item['name'].lower() == exe.name.lower() else next(
            root / item['name'] for root in (sdk / 'sdk/bin', sdk / 'oiio-deps/dist/bin')
            if (root / item['name']).is_file())
        if digest(original) != item['sha256']:
            raise ValueError('Runtime file changed: ' + item['name'])
        add('bin/' + item['name'], original)
    for component in sdk_receipt['sources']:
        source = sdk / component['destination']
        actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != component['commit']:
            raise ValueError('Notice source commit differs: ' + component['package'])
        tracked = subprocess.check_output(['git', '-C', str(source), 'ls-files', '-z']).decode('utf-8').split('\0')
        notices = []
        for relative in tracked:
            path = PurePosixPath(relative)
            if not relative or any(part.lower() in ('test', 'tests', 'testsuite', 'testimages', 'website', 'docs', 'doc') for part in path.parts[:-1]):
                continue
            if not (path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'AUTHORS', 'PATENTS', 'THIRD-PARTY')) or path.name == 'README.ijg'):
                continue
            original = source / relative
            if original.is_symlink() or not original.resolve().is_relative_to(source.resolve()) or original.stat().st_size > 2 * 1024 * 1024:
                raise ValueError('Unsupported notice source: ' + relative)
            # Notice bytes must come from the pinned Git object, not untracked
            # build output or an unnoticed modified working-tree license.
            committed = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:' + relative])
            if original.read_bytes() != committed:
                raise ValueError('Notice text differs from pinned source: ' + relative)
            destination = 'licenses/' + component['package'] + '/' + relative
            add(destination, committed)
            notices.append(dict(file=destination, sha256=hashlib.sha256(committed).hexdigest()))
        if not notices:
            raise ValueError('No notice material found: ' + component['package'])
        dynamic = any(item.get('package') == component['package'] for item in closure['files'])
        components.append(dict(name=component['package'], version=versions[component['package']],
                               commit=component['commit'], repository=component['repository'], notices=notices,
                               linkage='shared' if dynamic else 'static/header-only/build support; conservative acknowledgment'))
        # Preserve exact patches, including the upstream Zlib generated rename.
        patch = subprocess.check_output(['git', '-C', str(source), 'diff', '--binary'])
        if hashlib.sha256(patch).hexdigest() != component['source_patch_sha256']:
            raise ValueError('SDK source patch differs from receipt: ' + component['package'])
        if patch:
            add('provenance/patches/' + component['package'] + '.patch', patch)
    if not runtime_notices:
        raise ValueError('Supply the selected Visual Studio runtime notice materials')
    notices = []
    for original in sorted(runtime_notices):
        data = original.read_bytes()
        destination = 'licenses/MSVC-runtime/' + original.name
        add(destination, data)
        notices.append(dict(file=destination, sha256=hashlib.sha256(data).hexdigest()))
    components.append(dict(name='MSVC-runtime', version=Path(sdk_receipt['runtime']['source']).name,
                           source='Selected Visual Studio 2022 redistributable', notices=notices,
                           review='Original installed VS notice materials retained; runtime redistribution terms require final review.'))
    add('SOURCE-PROVENANCE.json', encode(dict(source_lock=sdk_receipt['source_lock'],
        source_lock_sha256=sdk_receipt['source_lock_sha256'], build_scripts_sha256=sdk_receipt['build_scripts_sha256'],
        nasm_sha256=sdk_receipt['nasm_sha256'], source_snapshot=evidence['source_snapshot'],
        source_commit_at_collection=evidence['source_commit_at_collection'], actual_identity=evidence['identity'],
        tests=evidence['ctest'], sdk_receipt_sha256=evidence['sdk_receipt_sha256'])))
    recipes = {name: digest(Path(__file__).parent / name) for name in ('package_runtime.py', 'runtime_closure.py', 'freeze_product.py')}
    manifest = dict(kind='internal-evaluation-candidate', redistribution_ready=False, project_license='not selected',
                    identity=evidence['identity'], executable_sha256=digest(exe), runtime_files=closure['files'],
                    system_dependencies=closure['system_dependencies'], components=components, package_recipes_sha256=recipes,
                    limitations=closure['limitations'], outstanding_requirements=[
                        'Project license decision', 'Final static/embedded notice and MSVC runtime distribution review',
                        'Minimum Windows 10 20H2/CPU validation and clean target-machine acceptance'])
    add('DEPENDENCIES.json', encode(manifest))
    add('README.txt', b'INTERNAL EVALUATION CANDIDATE\n\nRun bin/thumbgen.exe --version --json or bin/thumbgen.exe capabilities --json.\nProject license not selected. Redistribution is not approved.\nSee DEPENDENCIES.json and SOURCE-PROVENANCE.json for scope and outstanding gates.\nNo Python, compiler or SDK is required to run the packaged program.\n')
    hashes = {name: digest(value) if isinstance(value, Path) else hashlib.sha256(value).hexdigest() for name, value in payload.items()}
    add('SHA256SUMS', ''.join(hashes[name] + '  ' + name + '\n' for name in sorted(hashes)).encode())
    fingerprint = hashlib.sha256(encode(hashes)).hexdigest()
    version = re.sub(r'[^A-Za-z0-9_.-]', '_', evidence['identity']['version'])
    name = f'thumbgen-{version}-windows-x64-{digest(exe)[:12]}-r{fingerprint[:12]}'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.candidate-', suffix='.zip.tmp', dir=output, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for relative in sorted(payload):
                member = zipfile.ZipInfo(name + '/' + relative, date_time=(1980, 1, 1, 0, 0, 0))
                member.create_system = 3
                member.external_attr = 0o100644 << 16
                member.compress_type = zipfile.ZIP_DEFLATED
                value = payload[relative]
                data = value.read_bytes() if isinstance(value, Path) else value
                if relative in hashes and hashlib.sha256(data).hexdigest() != hashes[relative]:
                    raise ValueError('Payload changed during packaging: ' + relative)
                archive.writestr(member, data, compresslevel=9)
        target = output / (name + '.zip')
        checksum = digest(temporary)
        try:
            os.link(temporary, target)  # Atomic creation, never overwrite.
        except FileExistsError:
            if digest(target) != checksum:
                raise ValueError('Existing candidate differs; left unchanged')
        sidecar = target.with_suffix('.zip.sha256')
        text = checksum + '  ' + target.name + '\n'
        try:
            with sidecar.open('x', encoding='ascii', newline='\n') as stream:
                stream.write(text)
        except FileExistsError:
            if sidecar.read_text(encoding='ascii') != text:
                raise ValueError('Existing checksum sidecar differs; left unchanged')
        return dict(archive=str(target), sha256=checksum, files=len(payload), components=len(components), redistribution_ready=False)
    finally:
        temporary.unlink(missing_ok=True)  # Only this invocation's private file.


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--runtime-notice', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(pack(args.exe.resolve(), args.sdk.resolve(), args.evidence.resolve(), args.output.resolve(), args.runtime_notice), indent=2))
