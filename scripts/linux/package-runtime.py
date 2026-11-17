"""Create a deterministic INTERNAL candidate from an explicitly validated SDK stage.

No rebuild, Python runtime, test executables, compiler or system glibc is shipped.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from runtime_overlays import audit as audit_overlays, audit_base, audit_stage, require, validate_component_inventory

parser = argparse.ArgumentParser()
parser.add_argument('--validation', required=True, type=Path)
parser.add_argument('--cache', default='/home/builder/thumbgen-cache', type=Path)
parser.add_argument('--output', type=Path)
parser.add_argument('--source', type=Path, help='Frozen project archive source directory')
parser.add_argument('--source-archive', type=Path)
parser.add_argument('--archive-receipt', type=Path)
parser.add_argument('--tools-cache', type=Path)
parser.add_argument('--notice-materials', type=Path)
parser.add_argument('--work', type=Path)
parser.add_argument('--color-root', required=True, type=Path)
parser.add_argument('--core-root', required=True, type=Path)
parser.add_argument('--product-build', required=True, type=Path)
parser.add_argument('--dwa-root', type=Path, help='Optional verified portable-DWA build root')
args = parser.parse_args()
repo = (args.source or Path(__file__).resolve().parents[2]).resolve()
stage, cache = args.validation.resolve(), args.cache.resolve()
output = (args.output or repo / '.deps/linux/packages').resolve()
output.mkdir(parents=True, exist_ok=True)
git_command = '/opt/rh/rh-git227/root/usr/bin/git' if Path('/opt/rh/rh-git227/root/usr/bin/git').is_file() else 'git'
evidence = json.loads((stage / 'build-evidence.json').read_text())
lock = json.loads((repo / 'scripts/linux/sources-lock.json').read_text())
require(evidence['source_dependencies'] == lock['dependencies'], 'Stage dependency inventory differs from source lock')
base = audit_base(repo, cache)
overlays = audit_overlays(repo, args.color_root, args.core_root, args.product_build, args.dwa_root, base)
require(overlays == evidence['overlays'], 'Overlay provenance changed since product freeze')
require(base == evidence['original_sdk'], 'Original SDK evidence changed since freeze')
audit_stage(stage, args.product_build, cache, overlays, (args.tools_cache or cache) / 'tools/cmake-3.31.8-linux-x86_64/bin/cmake', base)
for item in evidence['copied']:
    require(hashlib.sha256(Path(item['source']).read_bytes()).hexdigest() == item['sha256'], 'Original linked library changed')
for filename, expected in evidence['product_install']['files'].items():
    require(hashlib.sha256(Path(filename).read_bytes()).hexdigest() == expected, 'Product build/install evidence changed')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def run(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
exe = stage / 'bin/thumbgen'
if digest(exe) != evidence['staged_artifacts']['bin/thumbgen']:
    raise SystemExit('Executable differs from the validated evidence')
for relative, expected in evidence['staged_artifacts'].items():
    if relative.startswith('lib/') and digest(stage / relative) != expected:
        raise SystemExit('Library differs from validated evidence: ' + relative)
identity = json.loads(run(str(exe), '--version', '--json'))
if identity['build'] != evidence['build_identity']:
    raise SystemExit('Build identity differs from validated evidence')
revision = evidence['product_source']['commit']
archive_binding = None
if identity['build']['revision'] == 'unknown':
    if not args.source or not args.source_archive:
        raise SystemExit('Unknown embedded revision requires explicit frozen source and source archive')
    receipt = json.loads((args.archive_receipt or stage / 'archive-source.json').read_text())
    if digest(args.source_archive) != receipt['archive_sha256'] or receipt['commit'] != revision:
        raise SystemExit('Archive checksum/commit differs from frozen evidence')
    with tarfile.open(args.source_archive) as source_archive:
        if source_archive.pax_headers.get('comment', '').strip() != revision:
            raise SystemExit('Git archive commit differs')
        archive_hashes = {}
        for member in source_archive.getmembers():
            relative = Path(member.name)
            if relative.is_absolute() or '..' in relative.parts:
                raise SystemExit('Unsafe source archive path')
            if member.isfile():
                archive_hashes[member.name] = hashlib.sha256(source_archive.extractfile(member).read()).hexdigest()
    if archive_hashes != receipt['sha256'] or archive_hashes != evidence['product_source']['sha256']:
        raise SystemExit('Full archive input map differs from compiled-source evidence')
    if (repo / '.git').exists() or any(digest(repo / path) != sha for path, sha in archive_hashes.items()):
        raise SystemExit('Frozen archive source was modified')
    archive_binding = dict(commit=revision, archive_sha256=receipt['archive_sha256'], input_files=len(archive_hashes),
                           input_map_sha256=hashlib.sha256(json.dumps(archive_hashes, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
                           embedded_revision='unknown', verification='Git archive bytes, full input map, frozen source tree and validated ELF SHA256')
elif not revision.startswith(identity['build']['revision']):
    raise SystemExit('Embedded revision and frozen source evidence disagree')
recipe_files = [Path(__file__).with_name(n) for n in ('package-runtime.py', 'runtime_overlays.py', 'runtime_native.py', 'runtime_aliases.py', 'runtime-notice-inventory.json')]
recipe_inputs = {p.name: digest(p) for p in recipe_files}
recipe_sha256 = hashlib.sha256(json.dumps(recipe_inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
name = 'thumbgen-' + re.sub(r'[^A-Za-z0-9_.-]', '_', identity['version']) + '-linux-x86_64-glibc217-' + revision[:12] + '-' + digest(exe)[:12] + '-r' + recipe_sha256[:8]
system = ('libc.so', 'libm.so', 'libdl.so', 'libpthread.so', 'librt.so', 'libutil.so',
          'libresolv.so', 'ld-linux-', 'libnss_', 'libanl.so', 'libcrypt.so', 'libnsl.so')
families = {'ZLIB':'Zlib', 'Imath':'BSD-3-Clause', 'OpenEXR':'BSD-3-Clause plus bundled notices',
            'libjpeg-turbo':'IJG, BSD-3-Clause, Zlib', 'TIFF':'libtiff license', 'PNG':'libpng-2.0',
            'WebP':'BSD-3-Clause and patent grant', 'OpenColorIO':'BSD-3-Clause', 'fmt':'MIT',
            'Robinmap':'MIT', 'libdeflate':'MIT', 'zstd':'BSD-3-Clause OR GPL-2.0 (BSD text retained)',
            'pystring':'BSD-3-Clause', 'expat':'MIT', 'yaml-cpp':'MIT', 'minizip-ng':'Zlib plus bundled notices'}
work = (args.work or output / 'package-work').resolve()
work.mkdir(parents=True, exist_ok=True)
notice_materials = (args.notice_materials or cache / 'runtime-license-rpm').resolve()
materials_receipt = notice_materials / 'NOTICE-MATERIALS.json'
materials = json.loads(materials_receipt.read_text()) if materials_receipt.exists() else None
if materials:
    if materials['rpm_sha256'] != 'cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa':
        raise SystemExit('Runtime notice RPM identity differs')
    for relative, sha in materials['files'].items():
        if digest(notice_materials / relative) != sha:
            raise SystemExit('Runtime notice material changed: ' + relative)
elif args.notice_materials:
    raise SystemExit('Explicit notice materials need a verified per-file receipt')
with tempfile.TemporaryDirectory(prefix='candidate-', dir=work) as temporary:
    root = Path(temporary) / name
    for directory in ('bin', 'lib', 'licenses'):
        (root / directory).mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, root / 'bin/thumbgen')
    branding = root / 'share/ThumbGen/branding'
    branding.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / 'docs/assets/branding/thumbgen-icon-concept-v1.png', branding / 'thumbgen.png')
    for size in (16, 24, 32, 48, 64, 128, 256, 512):
        icon_dir = root / f'share/icons/hicolor/{size}x{size}/apps'
        icon_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / f'resources/icons/{size}x{size}/thumbgen.png', icon_dir / 'thumbgen.png')
    # Recompute the executable's closure: the validation closure also contains
    # two test executables, which must never be copied into a runtime package.
    queue, seen, external = [exe.resolve()], set(), []
    while queue:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.add(path)
        dynamic = run('readelf', '-d', str(path))
        if '(NEEDED)' not in dynamic:
            continue
        loader = run('ldd', str(path))
        if 'not found' in loader:
            raise SystemExit('Unresolved dependency: ' + loader)
        for text in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader):
            dependency = Path(text).resolve()
            if dependency.is_file():
                # ldd on the executable reports the loader's complete closure.
                # Re-running ldd with a library as a new root would discard the
                # executable's inherited RPATH and falsely select host libraries.
                seen.add(dependency)
    for path in sorted(seen):
        if path == exe.resolve():
            continue
        dynamic = run('readelf', '-d', str(path))
        match = re.search(r'\(SONAME\).*?\[(.*?)\]', dynamic)
        soname = match[1] if match else path.name
        if soname.startswith(system) or path.name.startswith('ld-'):
            external.append(dict(soname=soname, requirement='provided by target OS; never bundled'))
            continue
        if path.parent != (stage / 'lib').resolve():
            raise SystemExit('Dependency escaped the validated stage: ' + str(path))
        shutil.copy2(path, root / 'lib' / path.name)
        if soname != path.name:
            (root / 'lib' / soname).symlink_to(path.name)
    cmake = (args.tools_cache or cache) / 'tools/cmake-3.31.8-linux-x86_64/bin/cmake'
    edits = []
    abi = []
    for path in sorted([root / 'bin/thumbgen', *[p for p in (root / 'lib').iterdir() if not p.is_symlink()]]):
        dynamic = run('readelf', '-d', str(path))
        old = re.search(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', dynamic)
        if old:
            desired = '$ORIGIN/../lib' if path.parent.name == 'bin' else '$ORIGIN'
            edits.append(f'file(RPATH_CHANGE FILE "{path}" OLD_RPATH "{old[1]}" NEW_RPATH "{desired}")')
        versions = run('readelf', '--version-info', '--wide', str(path))
        needs = versions.split('Version needs section', 1)[-1] if 'Version needs section' in versions else ''
        required = sorted(set(re.findall(r'Name: ((?:GLIBCXX|GLIBC|CXXABI)_[A-Za-z0-9_.]+)', needs)))
        for version in required:
            if version.startswith('GLIBC_') and (not re.fullmatch(r'GLIBC_[0-9.]+', version) or tuple(map(int, version[6:].split('.'))) > (2, 17)):
                raise SystemExit('Bundled ELF exceeds baseline: ' + version)
        abi.append(dict(file=str(path.relative_to(root)), needed=re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic), required_versions=required))
    edit_script = Path(temporary) / 'rpath.cmake'
    edit_script.write_text('\n'.join(edits) + '\n')
    subprocess.check_call([str(cmake), '-P', str(edit_script)])
    components = []
    skcms = json.loads((repo / 'dependencies/versions.json').read_text())['skcms']
    for relative, expected in skcms['files'].items():
        if digest(repo / 'third_party/skcms' / relative) != expected:
            raise SystemExit('Vendored skcms source differs: ' + relative)
    skcms_notice = root / 'licenses/skcms/LICENSE'
    skcms_notice.parent.mkdir(parents=True)
    shutil.copyfile(repo / 'dependencies/notices/skcms-BSD-3-Clause.txt', skcms_notice)
    if digest(skcms_notice) != skcms['files']['LICENSE']:
        raise SystemExit('skcms notice differs')
    components.append(dict(name='skcms', commit=skcms['revision'], repository=skcms['repository'],
        license=skcms['license'], linkage='static; x64 baseline with runtime AVX2/F16C; AVX-512 disabled',
        notices=[dict(file=skcms_notice.relative_to(root).as_posix(), sha256=digest(skcms_notice))]))
    stb = json.loads((repo / 'dependencies/versions.json').read_text())['stb_image_resize']
    if digest(repo / 'third_party/stb/stb_image_resize2.h') != stb['header_sha256']:
        raise SystemExit('Vendored resizer differs from dependency lock')
    stb_notice = root / 'licenses/stb_image_resize/LICENSE'
    stb_notice.parent.mkdir(parents=True)
    shutil.copyfile(repo / 'dependencies/notices/stb-image-resize-2.50.txt', stb_notice)
    components.append(dict(name='stb_image_resize', version=stb['version'], commit=stb['revision'],
        repository=stb['repository'], license=stb['license'], linkage='static/header-only',
        notices=[dict(file=stb_notice.relative_to(root).as_posix(), sha256=digest(stb_notice))]))
    def notices(component, source, metadata):
        found = []
        if not source.is_dir():
            raise SystemExit('Missing license source: ' + str(source))
        for current, directories, files in os.walk(source):
            directories[:] = sorted(d for d in directories if d not in ('.git', 'tests', 'test', 'testsuite', 'testimages', 'docs', 'doc', 'website', '__pycache__'))
            for filename in sorted(files):
                upper = filename.upper()
                if not (upper.startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'AUTHORS', 'PATENTS', 'THIRD-PARTY')) or filename == 'README.ijg'):
                    continue
                original = Path(current) / filename
                if original.is_symlink() or original.stat().st_size > 2 * 1024 * 1024:
                    continue
                relative = original.relative_to(source)
                if (source / '.git').exists():
                    expected_content = subprocess.check_output([git_command, '-C', str(source), 'show',
                                                               metadata['commit'] + ':' + relative.as_posix()])
                    if original.read_bytes() != expected_content:
                        raise SystemExit('Notice differs from pinned Git source: ' + str(original))
                destination = root / 'licenses' / component / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(original, destination)
                found.append(dict(file=str(destination.relative_to(root)), sha256=digest(destination)))
        if not found:
            raise SystemExit('No license texts found for ' + component)
        components.append(dict(name=component, notices=found, **metadata))
    oiio_source = cache / 'oiio-source' if (cache / 'oiio-source').is_dir() else repo / '.deps/oiio-src'
    if run(git_command, '-c', 'safe.directory=' + str(oiio_source), '-C', str(oiio_source), 'rev-parse', 'HEAD').strip() != lock['oiio_commit']:
        raise SystemExit('OIIO license source identity mismatch')
    if args.dwa_root:
        oiio_source = args.dwa_root / 'OpenImageIO-source'
    notices('OpenImageIO', oiio_source, dict(version='3.2.1.2', commit=lock['oiio_commit'],
            repository='https://github.com/AcademySoftwareFoundation/OpenImageIO', license='Apache-2.0 plus BSD and embedded third-party notices', linkage='shared; embedded notices retained'))
    for dependency in lock['dependencies']:
        source = cache / 'oiio-deps' / dependency['package']
        if run(git_command, '-C', str(source), 'rev-parse', 'HEAD').strip() != dependency['commit']:
            raise SystemExit('Dependency license source mismatch: ' + dependency['package'])
        notices(dependency['package'], source, dict(version=dependency['version'], commit=dependency['commit'],
                repository=dependency['repository'], license=families[dependency['package']], linkage=('original SDK provenance retained; flat codec replaced by OpenEXR-portable-DWA' if args.dwa_root and dependency['package'] == 'OpenEXR' else 'static/header embedded or shared ZLIB; conservative nested notices')))
    json_source = cache / 'src/json'
    if run(git_command, '-C', str(json_source), 'rev-parse', 'HEAD').strip() != lock['nlohmann_json']['commit']:
        raise SystemExit('JSON license source mismatch')
    notices('nlohmann_json', json_source, dict(**lock['nlohmann_json'], repository='https://github.com/nlohmann/json', license='MIT', linkage='header-only'))
    for item in overlays['components']:
        metadata = dict(item)
        component, source = metadata.pop('name'), Path(metadata.pop('source'))
        patch = metadata.pop('patch', None)
        notices(component, source, metadata)
        if patch:
            target = root / 'licenses' / component / Path(patch).name
            shutil.copyfile(patch, target)
            components[-1]['notices'].append(dict(file=target.relative_to(root).as_posix(), sha256=digest(target)))
            components[-1]['modification_notice'] = ('Portable SSE2 DWA IDCT in flat OIIO; independent of bounded Deep Core' if component == 'OpenEXR-portable-DWA' else 'Request-owned bounded ZSTD decoding') + '; public patch retained verbatim alongside upstream notices'
    for component, rpm_name in (('GCC-runtime', 'libgcc'), ('GCC-C++-runtime', 'libstdc++'), ('GCC-build-headers-nonshared', 'devtoolset-11-libstdc++-devel')):
        metadata = run('rpm', '-q', '--qf', '%{NAME}|%{VERSION}-%{RELEASE}|%{SOURCERPM}|%{LICENSE}', rpm_name).strip().split('|')
        notice_source = notice_materials / 'usr/share/doc/libgcc-4.8.5'
        notices(component, notice_source, dict(version=metadata[1], source_rpm=metadata[2],
                repository='https://vault.centos.org/7.9.2009/', license=metadata[3], linkage='GCC runtime / compiler headers and nonshared support',
                notice_source='CentOS libgcc-4.8.5-44.el7.x86_64.rpm, signature verified; GPL texts and exception 3.1',
                notice_rpm_sha256='cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa',
                review='GPL texts and GCC Runtime Library Exception 3.1 retained; eligible compilation and corresponding-source obligations need release review'))
    manifest = dict(kind='internal-evaluation-candidate', redistribution_ready=False, project_license='Apache-2.0',
                    package_recipe_sha256=recipe_sha256,
                    packaging_recipe_inputs=recipe_inputs,
                    overlay_auditor_sha256=digest(Path(__file__).with_name('runtime_overlays.py')),
                    linux_overlay_provenance=overlays, original_sdk_provenance=evidence['original_sdk'],
                    product_install_binding=evidence['product_install'],
                    identity=identity, frozen_product_commit=revision, validated_executable_sha256=digest(exe),
                    packaged_executable_sha256=digest(root / 'bin/thumbgen'), archive_binding=archive_binding,
                    notice_material_receipt_sha256=digest(materials_receipt) if materials else None,
                    frozen_source_snapshot=evidence['product_source']['sha256'],
                    dependency_components=components, target_os_libraries=external,
                    validation_closure_objects=len(json.loads((stage / 'runtime-provenance.json').read_text())['audit']['objects']),
                    executable_closure_objects=len(seen), bundled_elf_objects=len(abi),
                    outstanding_requirements=['GCC Runtime Library Exception applicability and any corresponding-source delivery requirements',
                                              'Final review of embedded/vendored component notices and distribution obligations',
                                              'Clean-machine, physical minimum OS and CPU validation not performed'])
    validate_component_inventory(manifest, repo)
    embedded_groups = {
        'BSD-3-Clause': ['OIIO pre-July-2023 retained code', 'NVIDIA Gelato-derived code', 'Open Shading Language-derived code',
                        'OpenColorIO-derived code/configuration acknowledgments', 'OpenEXR/Imath-derived code', 'DPX reader/writer',
                        'KissFFT', 'FindOpenVDB.cmake (build-only acknowledgment)', 'Cryptomatte-derived code'],
        'BSD-2-Clause': ['xxHash'],
        'MIT': ['PugiXML', 'FarmHash', 'FindTBB.cmake (build-only acknowledgment)', 'fmt-derived code', 'UTF-8 decoder', 'fasthash'],
        'Apache-2.0': ['CTPL', 'Droid fonts (conservative acknowledgment)', 'LLVM-derived function_view', 'ninjatracing.py (build-only acknowledgment)'],
        'BSL-1.0': ['Boost-derived math functions'], 'Zlib': ['Base-64 encoder'],
        'Unlicense OR MIT where offered': ['gif.h (conservative acknowledgment)', 'bcdec.h'],
        'Public-domain dedication': ['lookup3', 'SHA-1 implementation']}
    manifest['embedded_acknowledgments'] = [dict(name=item, license=license_name,
            version='vendored snapshot at OIIO ' + lock['oiio_commit'],
            exact_notice='licenses/OpenImageIO/THIRD-PARTY.md',
            usage='conservative upstream acknowledgment inventory; not a claim every optional/build-only item is linked')
            for license_name, items in embedded_groups.items() for item in items]
    manifest['payload_symlinks'] = {str(path.relative_to(root)): os.readlink(path) for path in root.rglob('*') if path.is_symlink()}
    (root / 'DEPENDENCIES.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    (root / 'ABI_REQUIREMENTS.json').write_text(json.dumps(abi, indent=2, sort_keys=True) + '\n')
    shutil.copyfile(repo / 'LICENSE', root / 'LICENSE')
    (root / 'NOTICE').write_text('INTERNAL CANDIDATE ONLY. ThumbGen is Apache-2.0.\n'
        'Upstream licenses and validation scope are recorded in DEPENDENCIES.json.\n'
        'OpenImageIO and static/header dependencies are attributed in DEPENDENCIES.json.\n'
        'Verbatim upstream notices, licenses, copyright statements and patent texts are in licenses/.\n'
        'OpenImageIO THIRD-PARTY.md includes embedded components; additional nested notices are retained conservatively.\n'
        'OpenImageIO: Copyright Contributors to the OpenImageIO project.\n'
        'This software is based in part on the work of the Independent JPEG Group.\n'
        'ELF runtime search paths are normalized to package-relative locations; upstream source notices are retained.\n'
        'GCC GPL texts and Runtime Library Exception are included; release obligations remain under review.\n')
    (root / 'README.txt').write_text('Internal ThumbGen Linux x86_64 candidate, glibc >= 2.17.\n'
        'Run ./thumbgen --version --json or ./bin/thumbgen capabilities --json.\n'
        'Relative ELF library paths are embedded; no Python/Conda/compiler is needed to run.\n'
        'System glibc/loader are never bundled. External OCIO configurations remain user supplied.\n'
        'Read NOTICE and DEPENDENCIES.json; this is not a licensed public release or a clean-machine certification.\n')
    (root / 'thumbgen').write_text('#!/bin/sh\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\nexec "$root/bin/thumbgen" "$@"\n')
    for path in root.rglob('*'):
        if not path.is_symlink():
            path.chmod(0o755 if path.is_dir() or path in (root / 'thumbgen', root / 'bin/thumbgen') else 0o644)
    sums = [digest(path) + '  ' + str(path.relative_to(root)) for path in sorted(root.rglob('*')) if path.is_file()]
    (root / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
    temporary_archive = Path(temporary) / (name + '.tar.gz')
    with temporary_archive.open('wb') as stream, gzip.GzipFile(filename='', mode='wb', fileobj=stream, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
            for path in [root, *sorted(root.rglob('*'))]:
                info = archive.gettarinfo(str(path), arcname=str(path.relative_to(root.parent)))
                info.uid = info.gid = 0
                info.uname = info.gname = ''
                info.mtime = 0
                info.pax_headers = {}
                with path.open('rb') if info.isfile() else open(os.devnull, 'rb') as content:
                    archive.addfile(info, content if info.isfile() else None)
    destination = output / temporary_archive.name
    checksum = digest(temporary_archive)
    if destination.exists() and digest(destination) != checksum:
        raise SystemExit('Same candidate inputs produced different archive bytes; existing file preserved')
    if not destination.exists():
        shutil.copyfile(temporary_archive, destination)
    Path(str(destination) + '.sha256').write_text(checksum + '  ' + destination.name + '\n')
    print(json.dumps(dict(archive=str(destination), sha256=checksum, components=len(components),
                          executable_closure=len(seen), bundled_elf=len(abi), redistribution_ready=False)))
