"""Local Linux provenance checks, not a signed supply-chain attestation.

Receipts are corroborated against locked Git bytes, actual build products,
install manifests, build recipes and the product link graph. No receipt hash
or build machine's absolute path is a permanent allowlist.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import io
import shutil
import importlib.util

_native_spec = importlib.util.spec_from_file_location('linux_runtime_native', Path(__file__).with_name('runtime_native.py'))
_native = importlib.util.module_from_spec(_native_spec)
_native_spec.loader.exec_module(_native)


SYSTEM = ('libc.so.', 'libm.so.', 'libdl.so.', 'libpthread.so.', 'librt.so.',
          'libutil.so.', 'libresolv.so.', 'ld-linux-', 'libnss_', 'libanl.so.')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(*args):
    return subprocess.check_output(list(map(str, args)), stderr=subprocess.STDOUT, text=True).strip()


def system_elf(path):
    path = Path(path).resolve()
    if str(path.parent) not in ('/usr/lib64', '/lib64', '/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu'):
        return False
    match = re.search(r'\(SONAME\).*?\[(.*?)\]', run('readelf', '-d', path))
    return bool(match and match[1].startswith(SYSTEM))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_component_inventory(manifest, repo):
    """Complete component identities and verbatim notices are verifier inputs."""
    repo = Path(repo)
    lock = json.loads((repo / 'scripts/linux/sources-lock.json').read_text())
    inventory = json.loads(Path(__file__).with_name('runtime-notice-inventory.json').read_text())['components']
    expected = {x['package'] for x in lock['dependencies']} | {'OpenImageIO', 'nlohmann_json',
        'GCC-runtime', 'GCC-C++-runtime', 'GCC-build-headers-nonshared', 'LittleCMS2-shared', 'JPEG-shared', 'OpenEXRCore-patched'}
    require(set(inventory) == expected, 'Verifier notice catalog does not cover source lock')
    locked = {x['package']: x for x in lock['dependencies']}
    locked.update(OpenImageIO=dict(commit=lock['oiio_commit']), nlohmann_json=lock['nlohmann_json'])
    for name, identity in locked.items():
        require(all(inventory[name].get(k) == identity[k] for k in ('commit', 'version') if k in identity), 'Notice catalog source identity differs: ' + name)
    components = manifest['dependency_components']
    require(len(components) == len(expected) and {c['name'] for c in components} == expected,
            'Component inventory differs from complete source lock')
    for item in components:
        approved = inventory[item['name']]
        require(all(item.get(k) == approved[k] for k in ('commit', 'version') if approved.get(k) is not None),
                'Component identity differs: ' + item['name'])
        require(sorted(item['notices'], key=lambda n: n['file']) == approved['notices'],
                'Component notice inventory differs: ' + item['name'])


def cache_values(build):
    result = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=')[0]:
            key, value = line.split('=', 1)
            if key.endswith(':BOOL'):
                value = 'OFF' if value.upper() in ('0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND', '') else 'ON'
            result[key.split(':')[0]] = value
    return result


def locked_tree(source, commit, patch=None, renames=None):
    require(run('git', '-C', source, 'rev-parse', 'HEAD') == commit, 'Source commit differs: ' + str(source))
    # Compare *all* tracked bytes to an independently materialized Git tree.
    # Applying the public patch here does not touch the supplied source cache.
    data = subprocess.check_output(['git', '-C', str(source), 'archive', commit])
    with tempfile.TemporaryDirectory(prefix='locked-linux-source-') as temporary:
        expected = Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            archive.extractall(expected)
        if patch:
            run('git', '-C', expected, 'apply', patch.resolve())
        files = [p for p in expected.rglob('*') if p.is_file()]
        def actual(p):
            relative = p.relative_to(expected).as_posix()
            if renames and relative in renames:
                require(not (source / relative).exists(), 'Expected upstream configure rename')
                relative = renames[relative]
            return source / relative
        require(all(actual(p).is_file() and sha(p) == sha(actual(p)) for p in files),
                'Tracked source bytes differ from public lock/patch: ' + str(source))
        extras = run('git', '-C', source, 'ls-files', '--others', '--exclude-standard').splitlines()
        require(not extras, 'Untracked source inputs: ' + str(extras))
        mapping = {p.relative_to(expected).as_posix(): sha(p) for p in files}
    return hashlib.sha256(json.dumps(mapping, sort_keys=True).encode()).hexdigest()


def build_evidence(build, source, prefix, options):
    values = cache_values(build)
    require(Path(values['CMAKE_HOME_DIRECTORY']).resolve() == source.resolve(), 'CMake source root differs')
    require(Path(values['CMAKE_INSTALL_PREFIX']).resolve() == prefix.resolve(), 'CMake install prefix differs')
    require(values['CMAKE_BUILD_TYPE'] == 'Release', 'Dependency is not Release')
    for key, value in options.items():
        require(values.get(key) == value, 'CMake option mismatch: ' + key)
    for key in ('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS'):
        if key == 'CMAKE_CXX_FLAGS' and not values.get('CMAKE_CXX_COMPILER'):
            continue  # The colour projects enable only C.
        flags = values.get(key, '')
        require('native' not in flags and '-march=x86-64' in flags and '-mtune=generic' in flags,
                'Missing portable CPU flags: ' + key)
    manifest = build / 'install_manifest.txt'
    require(manifest.is_file(), 'Missing install manifest')
    files = [build / 'CMakeCache.txt', manifest]
    files += [p for p in build.rglob('*') if p.is_file() and
              (p.name in ('link.txt', 'flags.make', 'build.make', 'DependInfo.cmake', 'build.ninja', 'cmake_install.cmake', 'compile_commands.json') or p.suffix == '.o')]
    require(any(p.name in ('link.txt', 'build.ninja') for p in files), 'Missing compile/link recipe')
    return dict(root=str(build), files={str(p): sha(p) for p in sorted(set(files))},
                installed_paths=manifest.read_text().splitlines(), options=values)


def installed_product(build, prefix, relative, receipt, evidence):
    path = prefix / relative
    require(not Path(relative).is_absolute() and '..' not in Path(relative).parts and
            path.resolve().is_relative_to(prefix.resolve()), 'Installed product escapes selected prefix')
    require(path.is_file() and sha(path) == receipt['installed_sha256'].get(relative), 'Installed digest differs: ' + relative)
    require((build / 'CMakeCache.txt').is_file(), 'Native build lacks configured source identity')
    values = cache_values(build)
    manifest = build / 'install_manifest.txt'
    require(manifest.is_file() and str(Path(values['CMAKE_INSTALL_PREFIX']) / relative) in manifest.read_text().splitlines(),
            'Installed artifact absent from CMake manifest: ' + str(path))
    candidate, native = _native.installed_origin(build, relative, values)
    def equivalent(candidate):
        if sha(candidate) == sha(path):
            return True
        if path.suffix == '.a':
            return False
        pattern = r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]'
        old = re.search(pattern, run('readelf', '-d', candidate))
        new = re.search(pattern, run('readelf', '-d', path))
        if not old:
            return False
        with tempfile.TemporaryDirectory(prefix='install-rpath-proof-') as directory:
            copy = Path(directory) / path.name
            shutil.copyfile(candidate, copy)
            script = Path(directory) / 'edit.cmake'
            script.write_text(f'file(RPATH_CHANGE FILE "{copy}" OLD_RPATH "{old[1]}" NEW_RPATH "{new[1]}")\n' if new else f'file(RPATH_REMOVE FILE "{copy}")\n')
            run(values['CMAKE_COMMAND'], '-P', script)
            return sha(copy) == sha(path)
    require(equivalent(candidate), 'Install is not the recorded build product: ' + relative)
    return dict(path=str(path.resolve()), sha256=sha(path), native=native)


def normalized_elf_sha(path, desired, cmake):
    match = re.search(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', run('readelf', '-d', path))
    if not match or match[1] == desired:
        return sha(path)
    with tempfile.TemporaryDirectory(prefix='runtime-elf-proof-') as directory:
        copy = Path(directory) / 'elf'
        shutil.copyfile(path, copy)
        script = Path(directory) / 'edit.cmake'
        script.write_text(f'file(RPATH_CHANGE FILE "{copy}" OLD_RPATH "{match[1]}" NEW_RPATH "{desired}")\n')
        run(cmake, '-P', script)
        return sha(copy)


def audit_stage(stage, build, cache, overlay, cmake):
    """Recompute linked closure; never accept a self-authored stage SHA map."""
    stage, build, cache = map(Path, (stage, build, cache))
    values = cache_values(build)
    install = Path(values['CMAKE_INSTALL_PREFIX']) / 'bin/thumbgen'
    expected = normalized_elf_sha(build / 'thumbgen', '$ORIGIN/../lib', cmake)
    require(expected == sha(install) == sha(stage / 'bin/thumbgen'), 'Stage/install not linked product bytes')
    loader = subprocess.check_output(['ldd', str(build / 'thumbgen')], env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}, text=True)
    require('not found' not in loader, 'Linked build closure unresolved')
    original = json.loads((cache / 'sdk-rebuild.json').read_text())['installed_sha256']
    commands = run('ninja', '-C', build, '-t', 'commands', 'thumbgen')
    static = {Path(p).resolve() for p in re.findall(r'/[^\s;]+\.a\b', commands)}
    require((cache / 'oiio-deps/dist/lib/libzstd.a').resolve() in static, 'Product must select the locked static ZSTD')
    patched = Path(overlay['evidence']['OpenEXRCore-patched']['product']['path']).resolve()
    for library in static - {patched}:
        require(library.is_relative_to(cache.resolve()) and original.get(library.relative_to(cache.resolve()).as_posix()) == sha(library),
                'Unknown or changed static link input: ' + str(library))
    allowed = {x['path']: x['sha256'] for x in overlay['shared'].values()}
    for item in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader):
        path = Path(item).resolve()
        if system_elf(path):
            continue
        digest = allowed.get(str(path))
        if path.is_relative_to(cache.resolve()):
            digest = original.get(path.relative_to(cache.resolve()).as_posix())
        if path.name.startswith(('libstdc++.so.', 'libgcc_s-')):
            package = run('rpm', '-qf', path)
            require(package.startswith(('libstdc++-4.8.5-', 'libgcc-4.8.5-')), 'Unknown runtime RPM')
            rows = [line.split() for line in run('rpm', '-q', '--dump', package).splitlines()]
            digest = next((r[3] for r in rows if Path(r[0]).resolve() == path and r[3] != '0' * 64), None)
        require(digest == sha(path), 'Unknown/changed linked dependency: ' + str(path))
        require(normalized_elf_sha(path, '$ORIGIN', cmake) == sha(stage / 'lib' / path.name), 'Staged library differs from linked dependency: ' + path.name)


def audit(repo, color, core, product_build):
    repo, color, core, product_build = map(Path, (repo, color, core, product_build))
    lock = json.loads((repo / 'dependencies/color-sdk.json').read_text())
    receipt = json.loads((color / 'receipt.json').read_text())
    components, evidence, shared = [], {}, {}
    for name, item in lock.items():
        source, build, prefix = color / (name + '-source'), color / (name + '-build'), color / 'sdk'
        require(next((r for r in receipt['components'] if r['package'] == name), None) == dict(package=name, **item, license_sha256=sha(source / item['license_file'])),
                'Colour receipt does not match public lock: ' + name)
        tree = locked_tree(source, item['commit'])
        options = dict(option.split('=', 1) for option in item['options'])
        proof = build_evidence(build, source, prefix, options)
        for notice in ([dict(path=item['notice'], sha256=item['notice_sha256'])] if name == 'LittleCMS2' else item['notices']):
            original = source / Path(notice['path']).name.replace('LittleCMS2-2.20.txt', 'LICENSE').replace('libjpeg-turbo-3.1.2-', '')
            bundled = repo / 'dependencies' / notice['path']
            # Repository notice locks preserve the original Windows download's
            # CRLF; the locked Git checkout uses LF. Preserve both raw hashes.
            require(sha(bundled) == notice['sha256'] and
                    original.read_bytes().replace(b'\r\n', b'\n') == bundled.read_bytes().replace(b'\r\n', b'\n'),
                    'Locked notice differs: ' + name)
        basename = 'liblcms2.so.2.20.0' if name == 'LittleCMS2' else 'libjpeg.so.8.3.2'
        matches = [p for p in prefix.rglob(basename) if p.is_file()]
        require(len(matches) == 1, 'Ambiguous colour library')
        product = installed_product(build, prefix, matches[0].relative_to(prefix).as_posix(), receipt, proof)
        shared[basename] = product
        evidence[name] = dict(tree_sha256=tree, build=proof, product=product,
                              raw={str(p): sha(p) for p in color.glob(name + '-*.log')})
        components.append(dict(name=name + '-shared', version=item['version'], commit=item['commit'],
                               repository=item['repository'], license=item['license'], linkage='shared direct product dependency', source=str(source)))
    receipt = json.loads((core / 'receipt.json').read_text())
    helper = repo / 'scripts/dependencies/build_bounded_openexr.py'
    patch = repo / 'dependencies/patches/openexr-bounded-zstd-v1.patch'
    # Read constants from the trusted archived builder, never execute a receipt.
    text = helper.read_text()
    commit = re.search(r"COMMIT = '([a-f0-9]+)'", text)[1]
    identity = re.search(r"IDENTITY = '([^']+)'", text)[1]
    helper_lf = helper.read_bytes().replace(b'\r\n', b'\n')
    helper_variants = {hashlib.sha256(data).hexdigest() for data in (helper_lf, helper_lf.replace(b'\n', b'\r\n'))}
    require(receipt['upstream'] == commit and receipt['runtime_identity'] == identity and
            receipt['patch_sha256'] == sha(patch) and receipt['helper_sha256'] in helper_variants, 'Core public builder/patch lock differs')
    tree = locked_tree(core / 'source', commit, patch)
    proof = build_evidence(core / 'build', core / 'source', core / 'prefix',
                           dict(BUILD_SHARED_LIBS='OFF', BUILD_TESTING='OFF', OPENEXR_FORCE_INTERNAL_DEFLATE='ON'))
    library = 'lib/libOpenEXRCore_v_3_5_0_OpenImageIO_v3_2.a'
    product = installed_product(core / 'build', core / 'prefix', library, receipt, proof)
    symbols = run('nm', '-g', '--defined-only', product['path'])
    require('exr_get_library_version' in symbols and 'internal_exr_apply_dwa' in symbols, 'Unexpected Core archive')
    product_cache = cache_values(product_build)
    require(Path(product_cache['OpenEXR_DIR']).resolve().is_relative_to((core / 'prefix').resolve()), 'Product selected another OpenEXR')
    commands = run('ninja', '-C', product_build, '-t', 'commands', 'thumbgen')
    require(product['path'] in commands, 'Product link does not include selected static Core')
    linked = {str(Path(p).resolve()) for p in re.findall(r'/[^\s;]+', commands) if Path(p).is_file()}
    require(all(item['path'] in linked for item in shared.values()), 'Product link does not include selected shared colour libraries')
    require(identity in run('strings', product_build / 'thumbgen'), 'Product lacks bounded Core runtime identity')
    evidence['OpenEXRCore-patched'] = dict(tree_sha256=tree, build=proof, product=product,
        helper_sha256=sha(helper), builder_executed_sha256=receipt['helper_sha256'], helper_eol_variants=sorted(helper_variants),
        patch_sha256=sha(patch), receipt_sha256=sha(core / 'receipt.json'),
        commands=receipt['commands'], raw={str(p): sha(p) for p in core.glob('*.log')})
    components.append(dict(name='OpenEXRCore-patched', version=identity, commit=commit,
        repository='https://github.com/AcademySoftwareFoundation/openexr', license='BSD-3-Clause plus bundled notices',
        linkage='static direct product dependency; original unpatched OpenEXR remains embedded in OIIO',
        source=str(core / 'source'), patch=str(patch), patch_sha256=sha(patch)))
    return dict(components=components, evidence=evidence, shared=shared,
                trust='Corroborated local source/build/install evidence; not signed supply-chain proof')


def audit_base(repo, cache):
    """Retain the original SDK's entire embedded dependency inventory."""
    repo, cache = Path(repo), Path(cache)
    lock = json.loads((repo / 'scripts/linux/sources-lock.json').read_text())
    old = json.loads((cache / 'sdk-rebuild.json').read_text())
    trees = {}
    for item in [dict(package='OpenImageIO', commit=lock['oiio_commit']), *lock['dependencies'],
                 dict(package='nlohmann_json', commit=lock['nlohmann_json']['commit'])]:
        name = item['package']
        source = cache / ('oiio-source' if name == 'OpenImageIO' else 'src/json' if name == 'nlohmann_json' else 'oiio-deps/' + name)
        # Pinned ZLIB CMakeLists.txt:72-76 moves the shipped zconf.h aside
        # before generating the build-specific header. Verify the saved bytes.
        trees[name] = locked_tree(source, item['commit'], renames={'zconf.h': 'zconf.h.included'} if name == 'ZLIB' else None)
    for relative, expected in old['installed_sha256'].items():
        require(sha(cache / relative) == expected, 'Original SDK installation changed: ' + relative)
    # Existing receipt is not enough: corroborate real build/install recipes,
    # source roots, raw successful logs and installed library products.
    records = {}
    for item in lock['dependencies']:
        name = item['package']
        build = cache / 'oiio-deps' / (name + '-build')
        log = cache / 'oiio-deps' / (name + '-build.log')
        text = log.read_text()
        require(all('==== ' + phase + ' (exit code 0)' in text for phase in ('configure', 'build', 'install')), 'Incomplete base build log: ' + name)
        subdirectory = {'expat': 'expat', 'zstd': 'build/cmake'}.get(name, '')
        require(Path(cache_values(build)['CMAKE_HOME_DIRECTORY']).resolve() == (cache / 'oiio-deps' / name / subdirectory).resolve(), 'Base build source differs: ' + name)
        records[name] = {str(p): sha(p) for p in [log, build / 'CMakeCache.txt', build / 'install_manifest.txt']}
    records['OpenImageIO'] = {str(p): sha(p) for p in [cache / 'oiio-build/CMakeCache.txt', cache / 'oiio-build/build.ninja',
        cache / 'oiio-build/install_manifest.txt', *sorted((cache / 'logs').glob('oiio-*.log'))]}
    # Installation authority comes from each configured native install graph,
    # not the caller's installed_sha256 list. Header-only projects have no
    # library rules; every actual base library must be accounted for below.
    products = {}
    specs = [(cache / 'oiio-build', cache / 'sdk')]
    specs += [(cache / 'oiio-deps' / (item['package'] + '-build'), cache / 'oiio-deps/dist') for item in lock['dependencies']]
    expected_paths = set()
    for build, prefix in specs:
        values = cache_values(build)
        manifest = build / 'install_manifest.txt'
        proof = dict(installed_paths=manifest.read_text().splitlines(), options=values)
        for rule in _native.install_rules(build):
            origin = Path(rule['origin'])
            if origin.is_symlink():
                continue  # SONAME alias; the real native output is mandatory.
            path = prefix / rule['relative']
            relative = path.relative_to(cache).as_posix()
            expected_paths.add(str(path))
            products[relative] = installed_product(build, prefix, rule['relative'],
                dict(installed_sha256={r.removeprefix(prefix.relative_to(cache).as_posix() + '/'): h
                    for r, h in old['installed_sha256'].items() if r.startswith(prefix.relative_to(cache).as_posix() + '/')}), proof)
    for prefix in (cache / 'sdk', cache / 'oiio-deps/dist'):
        for path in prefix.rglob('*'):
            if path.is_file() and (path.suffix == '.a' or '.so' in path.name) and path.name == path.resolve().name:
                require(str(path) in expected_paths, 'Unknown base library outside native install graph: ' + str(path))
    allowed = {p['path'] for p in products.values()} | {p['native']['output'] for p in products.values()}
    for product in products.values():
        for linked in product['native']['link_inputs']:
            path = Path(linked)
            if path.suffix == '.a' or '.so' in path.name:
                system_link = str(path.parent) in ('/usr/lib64', '/lib64') and re.fullmatch(r'lib(?:m|dl|pthread|rt|c|gcc_s|stdc\+\+)(?:\.so.*|-[0-9.]+\.so)', path.name)
                require(linked in allowed or system_link, 'Unknown native link library: ' + linked)
    return dict(source_trees=trees, builds=records, products=products, receipt_sha256=sha(cache / 'sdk-rebuild.json'))
