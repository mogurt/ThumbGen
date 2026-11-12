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
_alias_spec = importlib.util.spec_from_file_location('linux_runtime_aliases', Path(__file__).with_name('runtime_aliases.py'))
_aliases = importlib.util.module_from_spec(_alias_spec)
_alias_spec.loader.exec_module(_aliases)
artifact_kind = _aliases.artifact_kind
authorize_library = _aliases.authorize_library


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
    optional = {'OpenEXR-portable-DWA'}
    require(set(inventory) == expected | optional, 'Verifier notice catalog does not cover source lock')
    if 'portable_dwa' in manifest.get('linux_overlay_provenance', {}):
        expected |= optional
    locked = {x['package']: x for x in lock['dependencies']}
    locked.update(OpenImageIO=dict(commit=lock['oiio_commit']), nlohmann_json=lock['nlohmann_json'])
    for name, identity in locked.items():
        require(all(inventory[name].get(k) == identity[k] for k in ('commit', 'version') if k in identity), 'Notice catalog source identity differs: ' + name)
    # Vendored static components are versioned by the product source lock,
    # independently of the externally built SDK notice catalog.
    product_lock = json.loads((repo / 'dependencies/versions.json').read_text())
    for name, notice, expected_sha in (
        ('skcms', 'licenses/skcms/LICENSE', product_lock['skcms']['files']['LICENSE']),
        ('stb_image_resize', 'licenses/stb_image_resize/LICENSE', sha(repo / 'dependencies/notices/stb-image-resize-2.50.txt'))):
        expected.add(name)
        inventory[name] = dict(commit=product_lock[name]['revision'],
            notices=[dict(file=notice, sha256=expected_sha)])
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
        if artifact_kind(candidate) == 'archive':
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


def audit_installations(root, specs, installed_map):
    """Derive library/tool/alias authority only from actual native installs."""
    root = Path(root).resolve()
    products, tools, rules_by_prefix = {}, {}, {}
    for build, prefix in specs:
        build, prefix = Path(build).resolve(), Path(prefix).resolve()
        values = cache_values(build)
        rules = _native.install_rules(build)
        rules_by_prefix.setdefault(prefix, []).extend(rules)
        prefix_key = prefix.relative_to(root).as_posix()
        receipt = dict(installed_sha256={r.removeprefix(prefix_key + '/'): h for r, h in installed_map.items()
            if r.startswith(prefix_key + '/')} if prefix != root else dict(installed_map))
        for rule in rules:
            origin = Path(rule['origin'])
            if rule['kind'] == 'PROGRAM' and artifact_kind(origin) is None:
                continue  # generated SDK config scripts are not ELF/library authority
            if origin.is_symlink():
                continue  # verified below against its exact real native product
            path = prefix / rule['relative']
            relative = path.relative_to(root).as_posix()
            collection = tools if rule['kind'] in ('EXECUTABLE', 'PROGRAM') else products
            require(relative not in collection, 'Conflicting native install products: ' + relative)
            product = installed_product(build, prefix, rule['relative'], receipt, {})
            expected_kind = 'archive' if rule['kind'] == 'STATIC_LIBRARY' else 'elf'
            require(artifact_kind(path) == expected_kind, 'Installed native product has incorrect content type: ' + relative)
            collection[relative] = product
    libraries = {p['path']: p['sha256'] for p in products.values()}
    executables = {p['path']: p['sha256'] for p in tools.values()}
    native_products = {p['native']['output']: p['path'] for p in [*products.values(), *tools.values()]}
    aliases = {}
    for prefix, rules in rules_by_prefix.items():
        declared = {}
        for rule in rules:
            origin = Path(rule['origin'])
            if rule['kind'] == 'PROGRAM' and artifact_kind(origin) is None:
                continue
            if not origin.is_symlink():
                continue
            try:
                native_target = str(origin.resolve(strict=True))
            except (RuntimeError, OSError) as error:
                raise ValueError('Invalid native alias: ' + str(origin)) from error
            require(native_target in native_products, 'Native alias lacks a verified real install product: ' + str(origin))
            declared[str(prefix / rule['relative'])] = native_products[native_target]
        prefix_libraries = {p: h for p, h in libraries.items() if Path(p).is_relative_to(prefix)}
        prefix_tools = {p: h for p, h in executables.items() if Path(p).is_relative_to(prefix)}
        aliases.update(_aliases.audit_prefix(prefix, prefix_libraries, prefix_tools, declared))
    return dict(products=products, tools=tools, aliases=aliases, verified_files=libraries)


def check_native_link_inputs(installation):
    libraries = dict(installation['verified_files'])
    libraries.update({p['native']['output']: p['native']['output_sha256'] for p in installation['products'].values()})
    for product in [*installation['products'].values(), *installation['tools'].values()]:
        for linked in product['native']['link_inputs']:
            path = Path(linked)
            if artifact_kind(path):
                if not system_elf(path):
                    authorize_library(path, libraries)


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


def audit_stage(stage, build, cache, overlay, cmake, verified_base=None):
    """Recompute linked closure; never accept a self-authored stage SHA map."""
    stage, build, cache = map(Path, (stage, build, cache))
    values = cache_values(build)
    install = Path(values['CMAKE_INSTALL_PREFIX']) / 'bin/thumbgen'
    expected = normalized_elf_sha(build / 'thumbgen', '$ORIGIN/../lib', cmake)
    require(expected == sha(install) == sha(stage / 'bin/thumbgen'), 'Stage/install not linked product bytes')
    loader = subprocess.check_output(['ldd', str(build / 'thumbgen')], env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}, text=True)
    require('not found' not in loader, 'Linked build closure unresolved')
    base = verified_base if verified_base is not None else audit_base(Path(values['CMAKE_HOME_DIRECTORY']), cache)
    allowed = dict(base['verified_files'])
    allowed.update(overlay['verified_files'])
    _, inputs, _ = _native.query(build.resolve(), 'thumbgen')
    for name in inputs:
        path = Path(name) if Path(name).is_absolute() else build / name
        if artifact_kind(path) == 'elf':
            if path.suffix == '.o' and path.resolve().is_relative_to(build.resolve()):
                rule, _, _ = _native.query(build.resolve(), path.resolve().relative_to(build.resolve()).as_posix())
                require('_COMPILER' in rule, 'Product object lacks a native compiler rule')
            elif not system_elf(path):
                authorize_library(path, allowed)
    static = {(Path(p) if Path(p).is_absolute() else build / p).resolve() for p in inputs
        if artifact_kind(Path(p) if Path(p).is_absolute() else build / p) == 'archive'}
    require((cache / 'oiio-deps/dist/lib/libzstd.a').resolve() in static, 'Product must select the locked static ZSTD')
    patched = Path(overlay['evidence']['OpenEXRCore-patched']['product']['path']).resolve()
    for library in static - {patched}:
        if library.is_relative_to(build.resolve()):
            _native.target_proof(build.resolve(), library, Path(values['CMAKE_HOME_DIRECTORY']))
        else:
            authorize_library(library, allowed)
    for item in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader):
        path = Path(item).resolve()
        if system_elf(path):
            continue
        digest = allowed.get(str(path))
        if path.name.startswith(('libstdc++.so.', 'libgcc_s-')):
            package = run('rpm', '-qf', path)
            require(package.startswith(('libstdc++-4.8.5-', 'libgcc-4.8.5-')), 'Unknown runtime RPM')
            rows = [line.split() for line in run('rpm', '-q', '--dump', package).splitlines()]
            digest = next((r[3] for r in rows if Path(r[0]).resolve() == path and r[3] != '0' * 64), None)
        require(digest == sha(path), 'Unknown/changed linked dependency: ' + str(path))
        require(normalized_elf_sha(path, '$ORIGIN', cmake) == sha(stage / 'lib' / path.name), 'Staged library differs from linked dependency: ' + path.name)


def audit(repo, color, core, product_build, dwa_root=None, verified_base=None):
    repo, color, core, product_build = map(Path, (repo, color, core, product_build))
    lock = json.loads((repo / 'dependencies/color-sdk.json').read_text())
    receipt = json.loads((color / 'receipt.json').read_text())
    components, evidence, shared = [], {}, {}
    color_install = audit_installations(color / 'sdk', [(color / (name + '-build'), color / 'sdk') for name in lock], receipt['installed_sha256'])
    check_native_link_inputs(color_install)
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
    core_install = audit_installations(core / 'prefix', [(core / 'build', core / 'prefix')], receipt['installed_sha256'])
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
    result = dict(components=components, evidence=evidence, shared=shared,
                installations=dict(color=color_install, core=core_install),
                verified_files={**color_install['verified_files'], **core_install['verified_files']},
                trust='Corroborated local source/build/install evidence; not signed supply-chain proof')
    if dwa_root is not None:
        require(verified_base is not None, 'DWA requires independently verified base dependencies')
        dwa = audit_dwa(repo, dwa_root, verified_base, product_build)
        result['portable_dwa'] = dwa
        result['components'].append(dwa['component'])
        result['components'][2]['linkage'] = 'static direct Deep dependency; flat OIIO independently embeds portable-DWA patched OpenEXR'
        result['verified_files'].update(dwa['installation']['verified_files'])
    return result


def audit_dwa(repo, root, base, product_build):
    """Verify a separate flat-codec prefix without trusting its binding summary."""
    require(isinstance(base, dict) and all(k in base for k in ('products', 'tools', 'verified_files')),
            'DWA requires independently verified base dependencies')
    repo, root, product_build = map(lambda p: Path(p).resolve(), (repo, root, product_build))
    lock = json.loads((repo / 'scripts/linux/sources-lock.json').read_text())
    exr = next(x for x in lock['dependencies'] if x['package'] == 'OpenEXR')
    patch = repo / 'dependencies/patches/openexr-portable-dwa-v1.patch'
    receipt = json.loads((root / 'receipt.json').read_text())
    helper = repo / 'scripts/dependencies/build_portable_dwa_sdk.py'
    helper_lf = helper.read_bytes().replace(b'\r\n', b'\n')
    helper_variants = {hashlib.sha256(data).hexdigest() for data in
        (helper_lf, helper_lf.replace(b'\n', b'\r\n'))}
    require(receipt.get('helper_sha256') in helper_variants and
            receipt.get('patch_sha256') == sha(patch) and receipt.get('inputs', {}).get('lock') == lock,
            'DWA public builder/patch/source lock differs')
    prefix = root / 'prefix'
    trees, builds = {}, {}
    options = {
        'OpenEXR': dict(BUILD_SHARED_LIBS='OFF', BUILD_TESTING='OFF', OPENEXR_BUILD_TOOLS='OFF',
            OPENEXR_BUILD_PYTHON='OFF', OPENEXR_FORCE_INTERNAL_DEFLATE='ON',
            OPENEXR_LIB_SUFFIX='_v_3_5_0_OpenImageIO_v3_2',
            OPENEXR_INTERNAL_IMF_NAMESPACE='OpenImageIO_v3_2_Imf__3_5_0', OPENEXR_NAMESPACE_CUSTOM='1',
            ILMTHREAD_INTERNAL_NAMESPACE='OpenImageIO_v3_2_IlmThread__3_5_0', ILMTHREAD_NAMESPACE_CUSTOM='1'),
        'OpenImageIO': dict(BUILD_SHARED_LIBS='ON', EMBEDPLUGINS='ON', USE_SIMD='sse2',
            OIIO_BUILD_TESTS='OFF', OIIO_BUILD_TOOLS='OFF', OIIO_USE_EXR_C_API='ON', OIIO_USE_HWY='OFF',
            OpenImageIO_BUILD_MISSING_DEPS='none', USE_PYTHON='OFF')}
    for name, commit in [('OpenEXR', exr['commit']), ('OpenImageIO', lock['oiio_commit'])]:
        source, build = root / (name + '-source'), root / (name + '-build')
        trees[name] = locked_tree(source, commit, patch if name == 'OpenEXR' else None)
        builds[name] = build_evidence(build, source, prefix, options[name])
    require(Path(cache_values(root / 'OpenImageIO-build')['OpenEXR_DIR']).resolve() ==
            prefix / 'lib/cmake/OpenEXR', 'Flat OIIO selected another OpenEXR')
    installation = audit_installations(prefix,
        [(root / (name + '-build'), prefix) for name in options], receipt['installed_sha256'])
    for relative, expected in receipt['installed_sha256'].items():
        path = prefix / relative
        require(not Path(relative).is_absolute() and '..' not in Path(relative).parts and
                path.resolve().is_relative_to(prefix), 'Unsafe DWA receipt path')
        require(path.is_file() and sha(path) == expected, 'DWA installation changed: ' + relative)
    # All external native link inputs must come from verified installs; SDK tools
    # never enter this collection. Static archives retain object-byte proofs.
    combined = dict(products={**base['products'], **installation['products']},
        tools={**base['tools'], **installation['tools']},
        verified_files={**base['verified_files'], **installation['verified_files']})
    check_native_link_inputs(combined)
    exr_outputs = {p['path'] for p in installation['products'].values()
        if p['native']['install_rule']['kind'] == 'STATIC_LIBRARY'}
    oiio = installation['products'].get('lib/libOpenImageIO.so.3.2.1')
    util = installation['products'].get('lib/libOpenImageIO_Util.so.3.2.1')
    require(oiio and util and exr_outputs, 'Missing configured portable-DWA libraries')
    embedded = exr_outputs & set(oiio['native']['link_inputs'])
    expected_embedded = {str(prefix / 'lib' / ('lib' + name + '_v_3_5_0_OpenImageIO_v3_2.a'))
        for name in ('OpenEXR', 'OpenEXRCore', 'Iex', 'IlmThread')}
    require(embedded == expected_embedded, 'Flat OIIO does not embed selected static OpenEXR')
    values = cache_values(product_build)
    require(Path(values['OpenImageIO_DIR']).resolve() == prefix / 'lib/cmake/OpenImageIO',
            'Product selected another OpenImageIO')
    _, inputs, _ = _native.query(product_build, 'thumbgen')
    linked = {str((Path(p) if Path(p).is_absolute() else product_build / p).resolve()) for p in inputs}
    require({oiio['path'], util['path']}.issubset(linked), 'Product does not link verified portable-DWA OIIO/Util')
    loader = subprocess.check_output(['ldd', str(product_build / 'thumbgen')],
        env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}, text=True)
    loaded = {str(Path(p).resolve()) for p in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader)}
    require('not found' not in loader and {oiio['path'], util['path']}.issubset(loaded),
            'Product loader does not select verified portable-DWA OIIO/Util')
    return dict(source_trees=trees, builds=builds, static_embedding=sorted(embedded),
        installed_static_not_embedded=sorted(exr_outputs - embedded), installation=installation, loader=re.sub(r"0x[0-9a-f]+", "ADDRESS", loader),
        patch_sha256=sha(patch), helper_sha256=sha(helper), builder_executed_sha256=receipt['helper_sha256'],
        receipt_sha256=sha(root / 'receipt.json'), raw={str(p): sha(p) for p in sorted(root.glob('*.std*'))},
        component=dict(name='OpenEXR-portable-DWA', version=exr['version'], commit=exr['commit'],
            repository=exr['repository'], license='BSD-3-Clause plus bundled notices',
            linkage='patched static OpenEXR including flat Core embedded in shared OIIO; independent of bounded Deep Core',
            source=str(root / 'OpenEXR-source'), patch=str(patch), patch_sha256=sha(patch)))


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
    specs = [(cache / 'oiio-build', cache / 'sdk')]
    specs += [(cache / 'oiio-deps' / (item['package'] + '-build'), cache / 'oiio-deps/dist') for item in lock['dependencies']]
    installation = audit_installations(cache, specs, old['installed_sha256'])
    check_native_link_inputs(installation)
    for relative, expected in old['installed_sha256'].items():
        require(not Path(relative).is_absolute() and '..' not in Path(relative).parts, 'Unsafe SDK receipt path')
        require(sha(cache / relative) == expected, 'Original SDK installation changed: ' + relative)
    return dict(source_trees=trees, builds=records, **installation, receipt_sha256=sha(cache / 'sdk-rebuild.json'))
