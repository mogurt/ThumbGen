"""Structural provenance for fresh local Windows builds, not signed attestation.

Trust starts in the caller's clean, tracked repository and local build tools.
Every check is reconstructed from the native build tree; an installed hash map
alone, or an unknown generic receipt, cannot establish this provenance profile.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def recipe_paths(kind):
    paths = ['scripts/dependencies/build_verified_overlay.py',
             'scripts/dependencies/overlay_provenance.py',
             'scripts/dependencies/windows_overlays.py']
    paths += (['dependencies/color-sdk.json', 'scripts/build-color-sdk.py'] if kind == 'color' else
              ['scripts/dependencies/build_bounded_openexr.py', 'dependencies/patches/openexr-bounded-zstd-v1.patch'])
    return paths


def trusted_recipes(policy, kind):
    paths = recipe_paths(kind)
    result = []
    for relative in paths:
        data = policy.safe(policy.REPO, relative).read_bytes()
        committed = policy.git(policy.REPO, 'show', 'HEAD:' + relative)
        if data.replace(b'\r\n', b'\n') != committed.replace(b'\r\n', b'\n'):
            raise ValueError('Uncommitted provenance recipe: ' + relative)
        if relative not in paths[:3]:
            result.append(dict(file=relative, lf_sha256=hashlib.sha256(committed.replace(b'\r\n', b'\n')).hexdigest(),
                               raw_sha256=hashlib.sha256(data).hexdigest()))
    return result


def producer(policy, kind):
    return dict(revision=policy.git(policy.REPO, 'rev-parse', 'HEAD').decode().strip(),
                files_lf_sha256={p: hashlib.sha256((policy.REPO / p).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
                                 for p in recipe_paths(kind)})


def verify_producer(policy, kind, recorded):
    if set(recorded['files_lf_sha256']) != set(recipe_paths(kind)) or not re.fullmatch('[0-9a-f]{40}', recorded['revision']):
        raise ValueError('Unknown overlay producer identity')
    for relative, expected in recorded['files_lf_sha256'].items():
        original = policy.git(policy.REPO, 'show', recorded['revision'] + ':' + relative)
        if hashlib.sha256(original.replace(b'\r\n', b'\n')).hexdigest() != expected:
            raise ValueError('Overlay producer script differs from recorded Git object')
    # This is an archival identity, not execution authentication. Current clean
    # policy/helpers plus a fresh reconstruction of native evidence are authority.
    return recorded


def helper_constants(policy):
    tree = ast.parse((policy.REPO / 'scripts/dependencies/build_bounded_openexr.py').read_text(encoding='utf-8'))
    return {node.targets[0].id: ast.literal_eval(node.value) for node in tree.body
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id in ('COMMIT', 'IDENTITY')}


def patched_blobs(blobs, patch):
    """Apply the trusted two-file unified patch without modifying the input Git DB."""
    result = dict(blobs)
    sections = patch.decode('utf-8').split('diff --git ')[1:]
    if not sections:
        raise ValueError('Empty bounded patch')
    for section in sections:
        lines = section.splitlines(keepends=True)
        header = lines[0].strip().split()
        if len(header) != 2 or header[0][2:] != header[1][2:]:
            raise ValueError('Unsupported bounded patch paths')
        relative = header[0][2:]
        original = blobs[relative].decode('utf-8').splitlines(keepends=True)
        output, cursor, offset = [], 0, 1
        while offset < len(lines):
            match = re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@', lines[offset])
            if not match:
                offset += 1
                continue
            start = int(match[1]) - 1
            if start < cursor:
                raise ValueError('Overlapping bounded patch hunks')
            output.extend(original[cursor:start]); cursor = start; offset += 1
            while offset < len(lines) and not lines[offset].startswith('@@ '):
                line = lines[offset]; offset += 1
                if line.startswith((' ', '-')):
                    if cursor >= len(original) or original[cursor] != line[1:]:
                        raise ValueError('Bounded patch context differs')
                    cursor += 1
                if line.startswith((' ', '+')):
                    output.append(line[1:])
        output.extend(original[cursor:])
        result[relative] = ''.join(output).encode('utf-8')
    return result


def source_tree(policy, source, commit, patch=None):
    policy.identity(source, commit)
    if policy.git(source, 'ls-files', '--others', '-z'):
        raise ValueError('Untracked or ignored source injection: ' + str(source))
    entries = policy.git(source, 'ls-tree', '-rz', '--full-tree', commit).split(b'\0')
    paths, objects, modes = [], [], {}
    for entry in entries:
        if not entry:
            continue
        header, relative = entry.split(b'\t', 1)
        mode, kind, object_id = header.split()
        if mode not in (b'100644', b'100755', b'120000') or kind != b'blob':
            raise ValueError('Unsupported source tree entry')
        name = relative.decode('utf-8')
        paths.append(name); objects.append(object_id); modes[name] = mode
    raw = subprocess.check_output(['git', '-C', str(source), 'cat-file', '--batch'], input=b'\n'.join(objects) + b'\n')
    blobs, offset = {}, 0
    for relative in paths:
        end = raw.index(b'\n', offset)
        size = int(raw[offset:end].split()[-1]); offset = end + 1
        blobs[relative] = raw[offset:offset + size]; offset += size + 1
    if patch:
        blobs = patched_blobs(blobs, patch)
    result = {}
    for relative, expected in blobs.items():
        path = source / relative
        if modes[relative] == b'120000' and path.is_symlink():
            actual = os.readlink(path).replace('\\', '/').encode('utf-8')
            if Path(actual.decode()).is_absolute() or not path.resolve().is_relative_to(source.resolve()) or actual != expected:
                raise ValueError('Locked source symlink differs or escapes tree: ' + relative)
            result[relative] = hashlib.sha256(actual).hexdigest()
            continue
        actual = policy.safe(source, relative).read_bytes()
        normalize = lambda b: b if b'\0' in b else b.replace(b'\r\n', b'\n')
        if normalize(actual) != normalize(expected):
            raise ValueError('Source content differs from locked tree/patch: ' + relative)
        result[relative] = hashlib.sha256(actual).hexdigest()
    return dict(commit=commit, files_sha256=result)


def cache_values(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        match = re.match(r'([^/#][^:=]*):[^=]*=(.*)$', line)
        if match:
            values[match[1]] = match[2]
    return values


def cache_check(values, source, prefix, options):
    wanted = dict(options, CMAKE_HOME_DIRECTORY=source.as_posix(), CMAKE_INSTALL_PREFIX=prefix.as_posix(),
                  CMAKE_GENERATOR='Visual Studio 17 2022', CMAKE_GENERATOR_PLATFORM='x64')
    for key, expected in wanted.items():
        if values.get(key, '').replace('\\', '/').casefold() != expected.replace('\\', '/').casefold():
            raise ValueError('Unexpected overlay CMake option: ' + key)
    for key in ('CMAKE_TOOLCHAIN_FILE', 'CMAKE_PROJECT_TOP_LEVEL_INCLUDES', 'CMAKE_PROJECT_INCLUDE',
                'CMAKE_PROJECT_INCLUDE_BEFORE', 'CMAKE_USER_MAKE_RULES_OVERRIDE', 'CMAKE_C_COMPILER_LAUNCHER',
                'CMAKE_CXX_COMPILER_LAUNCHER', 'CMAKE_C_LINKER_LAUNCHER', 'CMAKE_CXX_LINKER_LAUNCHER',
                'CMAKE_C_STANDARD_INCLUDE_DIRECTORIES', 'CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES',
                'CMAKE_INCLUDE_PATH', 'CMAKE_LIBRARY_PATH', 'CMAKE_PROGRAM_PATH', 'CMAKE_FIND_ROOT_PATH'):
        if values.get(key):
            raise ValueError('Unsupported overlay CMake injection: ' + key)
    # The public entry point exposes no custom compiler/linker flags. In
    # particular a forged cache must not introduce /FI, /arch, /LIBPATH or an
    # arbitrary define while retaining the lock's shared/static booleans.
    for key in ('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS_RELEASE'):
        allowed = ({'/md', '/o2', '/ob2', '/dndebug'} if key.endswith('_RELEASE') else
                   {'/dwin32', '/d_windows', '/w3', '/gr', '/ehsc'})
        if key in values and set(values[key].lower().split()) - allowed:
            raise ValueError('Unsupported overlay compiler flags: ' + key)
    for prefix_name in ('CMAKE_EXE_LINKER_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS', 'CMAKE_STATIC_LINKER_FLAGS'):
        for key in (prefix_name, prefix_name + '_RELEASE'):
            if set(values.get(key, '').lower().split()) - {'/machine:x64', '/incremental:no'}:
                raise ValueError('Unsupported overlay linker flags: ' + key)


def tlog_records(path):
    groups, key = {}, None
    for line in path.read_text(encoding='utf-16').splitlines():
        if line.startswith('^'):
            key = line[1:]; groups.setdefault(key, [])
        elif line:
            if key is None:
                raise ValueError('Malformed native tracking log: ' + str(path))
            groups[key].append(line)
    if not groups:
        raise ValueError('Empty native tracking log: ' + str(path))
    return groups


def install_map(policy, trace, source, build, prefix):
    mapped = {}
    for line in trace.read_text(encoding='utf-8').splitlines():
        row = json.loads(line)
        args = row.get('args', [])
        if row.get('cmd', '').lower() != 'file' or not args or args[0] != 'INSTALL':
            continue
        destination = Path(args[args.index('DESTINATION') + 1])
        if not destination.resolve().is_relative_to(prefix.resolve()):
            raise ValueError('Native install destination escapes prefix')
        if args[args.index('TYPE') + 1] == 'DIRECTORY':
            raise ValueError('Directory install is unsupported in this exact overlay profile')
        tail = args[args.index('FILES') + 1:]
        origins = []
        for item in tail:
            if item in ('RENAME', 'PERMISSIONS', 'CONFIGURATIONS', 'OPTIONAL', 'MESSAGE_LAZY', 'MESSAGE_NEVER'):
                break
            origins.append(Path(item))
        rename = args[args.index('RENAME') + 1] if 'RENAME' in args else None
        for origin in origins:
            if not origin.is_file() and 'OPTIONAL' in args:
                continue
            if not any(origin.resolve().is_relative_to(base.resolve()) for base in (source, build)):
                raise ValueError('Untrusted native install origin: ' + str(origin))
            target = destination / (rename or origin.name)
            relative = target.relative_to(prefix).as_posix()
            if policy.sha(origin) != policy.sha(policy.safe(prefix, relative)):
                raise ValueError('Installed bytes differ from native origin: ' + relative)
            entry = dict(origin=str(origin), sha256=policy.sha(origin))
            if relative.casefold() in {x.casefold() for x in mapped} and mapped.get(relative) != entry:
                raise ValueError('Ambiguous native installation: ' + relative)
            mapped[relative] = entry
    if not mapped:
        raise ValueError('Missing native installation map')
    return mapped


def native_build(policy, source, build, prefix, options, trace, dependency_files=None):
    cache = cache_values(build / 'CMakeCache.txt')
    cache_check(cache, source, prefix, options)
    if not {'CMAKE_C_FLAGS', 'CMAKE_C_FLAGS_RELEASE'} <= set(cache):
        raise ValueError('Missing native compiler flag configuration')
    bindings = {}
    def bind(path):
        bindings[path.relative_to(build).as_posix()] = policy.sha(path)
    bind(build / 'CMakeCache.txt')
    compiler_files = sorted((build / 'CMakeFiles').glob('*/CMakeCCompiler.cmake'))
    if len(compiler_files) != 1:
        raise ValueError('Ambiguous native compiler configuration')
    configuration = compiler_files[0].read_text(encoding='utf-8'); bind(compiler_files[0])
    compiler = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', configuration)
    if not compiler or 'set(CMAKE_C_COMPILER_ID "MSVC")' not in configuration:
        raise ValueError('Expected native MSVC compiler')
    compiler = Path(compiler[1])
    tools = {str(compiler.parent / name): policy.sha(compiler.parent / name) for name in ('cl.exe', 'link.exe', 'lib.exe')}
    vs = Path(cache['CMAKE_GENERATOR_INSTANCE'])
    windows = Path(os.environ.get('SystemRoot', 'C:/Windows'))
    allowed = (source, build, vs, Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits',
               windows / 'System32', windows / 'Globalization')
    allowed = tuple(base.resolve() for base in allowed)
    file_hashes = {}
    def native_sha(file):
        key = str(file).casefold()
        if key not in file_hashes:
            file_hashes[key] = policy.sha(file)
        return file_hashes[key]
    # Exact native tracking inputs/outputs, rather than accepting a DLL digest
    # invented by a caller. The selected local compiler/toolchain is a trust input.
    inputs, products, compile_sources = {}, {}, set()
    compiled_objects, linked_objects = set(), set()
    tlogs = [p for p in build.rglob('*.tlog') if p.is_file() and 'Release' in p.parts]
    if not tlogs:
        raise ValueError('Missing native Release tracking logs')
    for path in tlogs:
        bind(path)
        tracking_name = re.sub(r'^cl\.\d+\.', 'cl.', path.name.lower())
        if tracking_name not in ('cl.read.1.tlog', 'cl.write.1.tlog', 'link.read.1.tlog', 'link.write.1.tlog',
                                     'lib.read.1.tlog', 'lib.write.1.tlog', 'rc.read.1.tlog', 'rc.write.1.tlog'):
            if tracking_name not in ('lib-link.read.1.tlog', 'lib-link.write.1.tlog'):
                continue
        groups = tlog_records(path)
        reading = '.read.' in path.name.lower()
        for key, records in groups.items():
            for item in [*key.split('|'), *records]:
                file = Path(item)
                if not file.is_absolute() or not file.is_file():
                    raise ValueError('Missing/unsafe native input or output: ' + item)
                resolved_file = file.resolve()
                if (not any(resolved_file.is_relative_to(base) for base in allowed) and
                        (dependency_files or {}).get(str(resolved_file).casefold()) != native_sha(file)):
                    raise ValueError('Native dependency outside source/build/toolchain: ' + item)
                (inputs if reading else products)[str(file)] = native_sha(file)
                if tracking_name == 'cl.read.1.tlog' and file.suffix.lower() in ('.c', '.cpp', '.cxx'):
                    compile_sources.add(str(file))
                if tracking_name == 'cl.write.1.tlog' and item in records and file.suffix.lower() == '.obj':
                    compiled_objects.add(file.resolve())
                if path.name.lower() in ('link.read.1.tlog', 'lib-link.read.1.tlog') and file.suffix.lower() == '.obj':
                    linked_objects.add(file.resolve())
    if not compile_sources or not any(Path(p).suffix.lower() in ('.dll', '.lib') for p in products):
        raise ValueError('Missing native compilation/link products')
    if not linked_objects or not linked_objects <= compiled_objects:
        raise ValueError('Link/archive input object lacks native compilation record')
    commands = [p for p in tlogs if p.name.lower() == 'cl.command.1.tlog']
    if not commands:
        raise ValueError('Missing native compiler commands')
    for path in commands:
        data = path.read_text(encoding='utf-16')
        link_command = path.parent / 'link.command.1.tlog'
        # Upstream JPEG's standalone utilities use /MT; they are audited as
        # local helper outputs, never linked into or copied into the product.
        if link_command.is_file() and re.search(r'/OUT:"[^"]+\.EXE"', link_command.read_text(encoding='utf-16'), re.I):
            continue
        if re.search(r'(?i)(?:^|\s)/MT(?:d)?(?:\s|$)', data) or not re.search(r'(?i)(?:^|\s)/MD(?:\s|$)', data):
            raise ValueError('Overlay library requires the Release shared MSVC runtime: ' + str(path))
    # MSBuild's link.write log omits some /IMPLIB products. Accept those only
    # when the same real linker command's /OUT DLL is an observed write, and
    # bind the import library's bytes as a product of that command.
    for path in tlogs:
        if path.name.lower() != 'link.command.1.tlog':
            continue
        for lines in tlog_records(path).values():
            switches = {m[1].upper(): Path(m[2] or m[3]) for m in
                        re.finditer(r'/(OUT|IMPLIB):(?:"([^"]+)"|(\S+))', '\n'.join(lines), re.I)}
            output, implib = switches.get('OUT'), switches.get('IMPLIB')
            if output and output.suffix.lower() == '.dll' and implib:
                if (str(output) not in products or not implib.resolve().is_relative_to(build.resolve()) or
                        not implib.is_file()):
                    raise ValueError('Import library lacks associated native DLL output')
                products[str(implib)] = policy.sha(implib)
    for path in build.rglob('*'):
        if path.is_file() and (path.suffix in ('.vcxproj', '.props', '.targets') or path.name == 'cmake_install.cmake'
                              or path.name == 'install_manifest.txt'):
            bind(path)
    mapped = install_map(policy, trace, source, build, prefix)
    # Every installed PE/archive must be an observed compiler/linker output.
    observed = {Path(p).resolve() for p in products}
    for entry in mapped.values():
        origin = Path(entry['origin'])
        if origin.suffix.lower() in ('.dll', '.exe', '.lib') and origin.resolve() not in observed:
            raise ValueError('Installed binary lacks native production record: ' + str(origin))
    return dict(configuration=cache, compiler_tools_sha256=tools, files_sha256=bindings,
                input_files_sha256=inputs, output_files_sha256=products,
                compiled_sources=sorted(compile_sources), installation=mapped)


def build_commands(policy, kind, root, names):
    rows = policy.load(root / 'build-commands.json')
    if [row.get('name') for row in rows] != ['helper', *[name + '-install' for name in names]]:
        raise ValueError('Missing or unexpected actual overlay build commands')
    for row in rows:
        executable = Path(row['executable'])
        if row.get('exit') != 0 or policy.sha(executable) != row['executable_sha256']:
            raise ValueError('Failed command or changed local build tool')
    args = rows[0]['args']
    helper = policy.REPO / ('scripts/build-color-sdk.py' if kind == 'color' else 'scripts/dependencies/build_bounded_openexr.py')
    if Path(args[0]).resolve() != Path(rows[0]['executable']).resolve() or Path(args[1]).resolve() != helper.resolve():
        raise ValueError('Unexpected overlay helper executable')
    if len(args[2:]) % 2:
        raise ValueError('Invalid helper argument pairs')
    options = dict(zip(args[2::2], args[3::2]))
    allowed = {'--root', '--lcms-cache', '--jpeg-cache'} if kind == 'color' else {'--output', '--source-cache', '--deps-prefix'}
    if len(options) != len(args[2:]) // 2 or set(options) - allowed or Path(options['--root' if kind == 'color' else '--output']).resolve() != root.resolve():
        raise ValueError('Unexpected helper options/output')
    for row, name in zip(rows[1:], names):
        build = root / (name + '-build') if kind == 'color' else root / 'build'
        expected = ['cmake', '-DCMAKE_INSTALL_CONFIG_NAME=Release', '--trace-expand', '--trace-format=json-v1',
                    '--trace-redirect=' + str(root / (name + '-install-trace.jsonl')), '-P', str(build / 'cmake_install.cmake')]
        if row['args'] != expected:
            raise ValueError('Unexpected native install command')
    return rows, options


def dependency_sdk(policy, sdk):
    receipt = policy.load(sdk / 'sdk/thumbgen-sdk.json')
    if (receipt.get('kind') != 'thumbgen-windows-source-sdk' or
            receipt['source_lock'] != policy.load(policy.REPO / 'dependencies/windows-source-sdk.json')):
        raise ValueError('Bounded build dependency SDK lock differs')
    files = {}
    for relative, entry in receipt['artifacts'].items():
        path = policy.safe(sdk, relative)
        if policy.sha(path) != entry['sha256']:
            raise ValueError('Bounded build dependency SDK artifact differs: ' + relative)
        files[str(path.resolve()).casefold()] = entry['sha256']
    return dict(receipt_sha256=policy.sha(sdk / 'sdk/thumbgen-sdk.json'), files_sha256=files)


def observe(policy, kind, prefix):
    """Derive all authority paths; receipts cannot nominate arbitrary evidence files."""
    root = prefix.parent
    receipt = policy.load(root / 'receipt.json')
    recipes = trusted_recipes(policy, kind)
    lock = policy.load(policy.REPO / 'dependencies/color-sdk.json') if kind == 'color' else None
    components = list(lock) if lock else ['bounded']
    commands, helper_options = build_commands(policy, kind, root, components)
    dependencies = None
    if not lock:
        deps_prefix = Path(helper_options['--deps-prefix']).resolve()
        sdk = deps_prefix.parents[1]
        if deps_prefix != sdk / 'oiio-deps/dist':
            raise ValueError('Unexpected bounded build dependency prefix')
        dependencies = dependency_sdk(policy, sdk)
    result = dict(kind='thumbgen-overlay-build-v2', overlay=kind, recipes=recipes,
                  helper_receipt_sha256=policy.sha(root / 'receipt.json'), sources={}, builds={}, installed={})
    result['producer'] = verify_producer(policy, kind, policy.load(root / 'build-producer.json'))
    if dependencies:
        result['base_sdk'] = dependencies
    for name in components:
        source, build = ((root / (name + '-source'), root / (name + '-build')) if lock else (root / 'source', root / 'build'))
        if lock:
            commit, patch = lock[name]['commit'], None
            options = dict(item.split('=', 1) for item in lock[name]['options'])
            options.update(CMAKE_MSVC_RUNTIME_LIBRARY='MultiThreadedDLL', CMAKE_PREFIX_PATH='')
        else:
            commit = helper_constants(policy)['COMMIT']
            patch = (policy.REPO / 'dependencies/patches/openexr-bounded-zstd-v1.patch').read_bytes().replace(b'\r\n', b'\n')
            options = dict(BUILD_SHARED_LIBS='OFF', BUILD_TESTING='OFF', OPENEXR_BUILD_TOOLS='OFF',
                OPENEXR_INSTALL_TOOLS='OFF', OPENEXR_BUILD_EXAMPLES='OFF', OPENEXR_BUILD_PYTHON='OFF',
                OPENEXR_FORCE_INTERNAL_DEFLATE='ON', OPENEXR_LIB_SUFFIX='_v_3_5_0_OpenImageIO_v3_2',
                OPENEXR_INTERNAL_IMF_NAMESPACE='OpenImageIO_v3_2_Imf__3_5_0', OPENEXR_NAMESPACE_CUSTOM='1')
            options['CMAKE_PREFIX_PATH'] = str(deps_prefix)
        result['sources'][name] = source_tree(policy, source, commit, patch)
        trace = root / (name + '-install-trace.jsonl')
        native = native_build(policy, source, build, prefix, options, trace,
                              dependencies['files_sha256'] if dependencies else None)
        result['builds'][name] = native
        for relative, entry in native['installation'].items():
            if relative in result['installed'] and result['installed'][relative] != entry:
                raise ValueError('Multiple overlay install owners: ' + relative)
            result['installed'][relative] = entry
    if lock:
        for name in lock:
            for filename in (lock[name]['license_file'], *(['README.ijg'] if name == 'JPEG' else [])):
                relative = 'share/thumbgen-color-licenses/' + name + '/' + filename
                original = policy.safe(root / (name + '-source'), filename)
                if policy.sha(original) != policy.sha(policy.safe(prefix, relative)):
                    raise ValueError('Manual helper notice copy differs')
                result['installed'][relative] = dict(origin=str(original), sha256=policy.sha(original))
    derived = {name: entry['sha256'] for name, entry in result['installed'].items()}
    actual = policy.installed(prefix, derived)
    declared = policy.installed(prefix, receipt['installed_sha256'])
    if set(actual) != set(declared):
        raise ValueError('Helper receipt and native install set differ')
    result['evidence_sha256'] = {p.name: policy.sha(p) for p in sorted(root.glob('*.log'))}
    result['evidence_sha256'].update({p.name: policy.sha(p) for p in sorted(root.glob('*-install-trace.jsonl'))})
    result['commands'] = commands
    return result


def verify(policy, kind, prefix):
    recorded = policy.load(prefix.parent / 'overlay-provenance.json')
    if recorded.get('kind') != 'thumbgen-overlay-build-v2' or recorded.get('overlay') != kind:
        raise ValueError('Unknown overlay provenance profile')
    actual = observe(policy, kind, prefix)
    if actual != recorded:
        raise ValueError('Overlay native provenance differs from frozen build evidence')
    return actual
