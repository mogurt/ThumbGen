"""Read configured CMake install/native graphs without executing install or rebuild.

The accepted origin is a configured library target, never a recursive basename
search. Graph/source/object records are local corroboration, not execution proof.
"""
import hashlib
from pathlib import Path
import re
import shlex
import subprocess
from collections import Counter


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True, stderr=subprocess.STDOUT)


def local_file(path, root):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root),
            'Native target path escapes build or is a symlink: ' + str(path))
    # A symlinked parent below the canonical build root is also forbidden.
    require(path.absolute() == path.resolve(), 'Native target parent is redirected: ' + str(path))
    return path


def archive_objects(path):
    """Read every ar member, including duplicate basenames and GNU long names."""
    data = Path(path).read_bytes()
    require(data.startswith(b'!<arch>\n'), 'Unsupported native archive format')
    cursor, names, result = 8, b'', []
    while cursor < len(data):
        header = data[cursor:cursor+60]
        require(len(header) == 60 and header[58:60] == b'`\n', 'Malformed native archive')
        size = int(header[48:58]); name = header[:16].decode().strip()
        content = data[cursor+60:cursor+60+size]
        require(len(content) == size, 'Truncated native archive')
        cursor += 60 + size + size % 2
        if name == '//':
            names = content; continue
        if name in ('/', '/SYM64/'):
            continue
        if name.startswith('#1/'):
            length = int(name[3:]); name = content[:length].decode().rstrip('\0'); content = content[length:]
        elif re.fullmatch(r'/[0-9]+', name):
            offset = int(name[1:]); end = names.find(b'/\n', offset)
            require(end >= offset, 'Malformed native archive long name')
            name = names[offset:end].decode()
        else:
            name = name.rstrip('/')
        result.append((Path(name).name, hashlib.sha256(content).hexdigest()))
    return Counter(result)


def install_rules(build):
    build = Path(build).resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    prefix_match = re.search(r'^CMAKE_INSTALL_PREFIX:[^=]+=(.*)$', cache, re.M)
    require(prefix_match, 'Native install prefix is not configured')
    configured_prefix = Path(prefix_match[1])
    queue, seen, result = [build / 'cmake_install.cmake'], set(), []
    while queue:
        script = local_file(queue.pop(), build)
        if script in seen:
            continue
        seen.add(script)
        text = script.read_text()
        for included in re.findall(r'include\("([^"\n]*cmake_install\.cmake)"\)', text):
            require('$' not in included, 'Unresolved install include')
            queue.append(Path(included))
        for match in re.finditer(r'file\(INSTALL\s+DESTINATION\s+"([^"]+)"\s+TYPE\s+(STATIC_LIBRARY|SHARED_LIBRARY|EXECUTABLE|PROGRAM)\s+(?:RENAME\s+"([^"]+)"\s+)?(?:MESSAGE_(?:NEVER|LAZY|ALWAYS)\s+)?FILES\s+(.*?)\)', text, re.S):
            destination, kind, renamed, files = match.groups()
            if destination.startswith('${CMAKE_INSTALL_PREFIX}/'):
                relative_directory = destination[len('${CMAKE_INSTALL_PREFIX}/'):]
            else:
                require('$' not in destination and Path(destination).is_relative_to(configured_prefix), 'Library install destination escapes configured prefix')
                relative_directory = Path(destination).relative_to(configured_prefix).as_posix()
            require('$' not in relative_directory and '..' not in Path(relative_directory).parts, 'Unresolved/unsafe install destination')
            for value in re.findall(r'"([^"]+)"', files):
                require('$' not in value, 'Unresolved library install source')
                origin = Path(value)
                # SONAME aliases are ordinary CMake install inputs, but only
                # their resolved real target may establish a native origin.
                require(origin.resolve().is_relative_to(build), 'Install origin escapes native build')
                require(not renamed or (Path(renamed).name == renamed and renamed not in ('.', '..')), 'Unsafe install rename')
                result.append(dict(relative=(Path(relative_directory) / (renamed or origin.name)).as_posix(),
                    origin=str(origin), kind=kind, script=str(script), script_sha256=sha(script)))
    unique = {}
    for rule in result:
        key = (rule['relative'], rule['origin'], rule['kind'])
        if key not in unique:
            unique[key] = dict(rule, repeated_rules=[])
        else:
            unique[key]['repeated_rules'].append(dict(script=rule['script'], script_sha256=rule['script_sha256']))
    return list(unique.values())


def query(build, name):
    text = output('ninja', '-C', build, '-t', 'query', name)
    match = re.search(r'^  input: (\S+)\n(.*?)(?=^  outputs:|\Z)', text, re.M | re.S)
    require(match, 'Missing native input rule: ' + name)
    inputs = [line.strip().lstrip('|').strip() for line in match[2].splitlines() if line.strip()]
    return match[1], inputs, text


def target_proof(build, origin, source, kind='STATIC_LIBRARY'):
    build, origin, source = map(lambda p: Path(p).resolve(), (build, origin, source))
    local_file(origin, build)
    objects, sources, links, recipes = [], {}, {}, {}
    if (build / 'build.ninja').is_file():
        name = origin.relative_to(build).as_posix()
        rule, inputs, text = query(build, name)
        require(('_EXECUTABLE_LINKER' in rule) if kind in ('EXECUTABLE', 'PROGRAM') else ('_STATIC_LIBRARY_LINKER' in rule or '_SHARED_LIBRARY_LINKER' in rule),
                'Install source is not a configured native library target: ' + name)
        recipes['native_query_sha256'] = hashlib.sha256(text.encode()).hexdigest()
        recipes['commands_sha256'] = hashlib.sha256(output('ninja', '-C', build, '-t', 'commands', name).encode()).hexdigest()
        recipes[str(build / 'build.ninja')] = sha(build / 'build.ninja')
        for value in inputs:
            path = Path(value) if Path(value).is_absolute() else build / value
            if value.endswith('.o'):
                obj = local_file(path, build)
                objects.append(obj)
                compile_rule, dependencies, raw = query(build, value)
                require('_COMPILER' in compile_rule, 'Object is not a configured compiler target')
                candidates = [Path(v) if Path(v).is_absolute() else build / v for v in dependencies
                              if Path(v).suffix in ('.c', '.cc', '.cpp', '.cxx', '.s', '.S', '.asm')]
                require(len(candidates) == 1, 'Object lacks one native compile source: ' + value)
                compiled = candidates[0]
                require(compiled.is_file() and not compiled.is_symlink() and
                        (compiled.resolve().is_relative_to(source) or compiled.resolve().is_relative_to(build)),
                        'Compile source escapes configured source/build')
                sources[str(compiled)] = sha(compiled)
                recipes['query:' + value] = hashlib.sha256(raw.encode()).hexdigest()
            elif path.is_file():
                links[str(path.resolve())] = sha(path)
    else:
        directories = local_file(build / 'CMakeFiles/TargetDirectories.txt', build)
        matches = []
        for value in directories.read_text().splitlines():
            directory = Path(value)
            require(directory.resolve().is_relative_to(build), 'Configured target directory escapes native build')
            link = directory / 'link.txt'
            if not link.is_file():
                continue
            local_file(link, build)
            cwd = directory.parent.parent
            tokens = shlex.split(link.read_text().splitlines()[0])
            if '-o' in tokens:
                target = tokens[tokens.index('-o') + 1]
            elif Path(tokens[0]).name.endswith(('ar', 'gcc-ar')) and len(tokens) > 2:
                target = tokens[2]
            else:
                continue
            path = Path(target) if Path(target).is_absolute() else cwd / target
            if path.resolve() == origin:
                matches.append((directory, link, cwd, tokens))
        require(len(matches) == 1, 'Install source is not one configured native library target: ' + str(origin))
        directory, link, cwd, tokens = matches[0]
        info = local_file(directory / 'DependInfo.cmake', build)
        recipes.update({str(p): sha(p) for p in (directories, link, info, local_file(directory / 'build.make', build))})
        linked_objects = {(Path(v) if Path(v).is_absolute() else cwd / v).resolve() for v in tokens if v.endswith('.o')}
        # CMake object libraries (e.g. OpenEXR's vendored OpenJPH) contribute
        # objects from other *configured* targets without a separate archive.
        # Follow those explicit link inputs into TargetDirectories/DependInfo.
        for value in directories.read_text().splitlines():
            directory = Path(value)
            info = directory / 'DependInfo.cmake'
            if not info.is_file():
                continue
            local_file(info, build)
            # C/C++ depfile tuples have four fields; older ASM records use
            # source/object pairs. Both begin with the same explicit pair.
            pairs = re.findall(r'"([^"]+)"\s+"([^"]+\.o)"', info.read_text())
            for compiled, obj in pairs:
                obj = Path(obj) if Path(obj).is_absolute() else build / obj
                if obj.resolve() not in linked_objects:
                    continue
                local_file(obj, build)
                compiled = Path(compiled)
                require(compiled.is_file() and not compiled.is_symlink() and
                        (compiled.resolve().is_relative_to(source) or compiled.resolve().is_relative_to(build)),
                        'Compile source escapes configured source/build')
                require(obj not in objects, 'Duplicate configured object origin')
                objects.append(obj); sources[str(compiled)] = sha(compiled)
                for recipe in (info, directory / 'build.make', directory / 'flags.make'):
                    recipes[str(local_file(recipe, build))] = sha(recipe)
        require(linked_objects == {o.resolve() for o in objects}, 'Native object/compile graph mismatch: ' + str(link))
        for value in tokens[1:]:  # the compiler/ar executable is a tool, not a library input
            path = Path(value) if Path(value).is_absolute() else cwd / value
            if path.is_file() and path.resolve() not in linked_objects and path.resolve() != origin:
                links[str(path.resolve())] = sha(path)
    require(objects and sources, 'Native library has no compiled object/source inputs')
    if kind == 'STATIC_LIBRARY':
        require(archive_objects(origin) == Counter((obj.name, sha(obj)) for obj in objects),
                'Native archive object differs from configured compiler output: ' + str(origin))
    return dict(output=str(origin), output_sha256=sha(origin), objects={str(o): sha(o) for o in objects},
                sources=sources, link_inputs=links, recipes=recipes)


def installed_origin(build, relative, values):
    build = Path(build).resolve()
    require('CMAKE_HOME_DIRECTORY' in values, 'Native build lacks configured source identity')
    matches = [r for r in install_rules(build) if r['relative'] == relative]
    require(len(matches) == 1, 'Installation is not one configured native install output: ' + relative)
    rule = matches[0]
    origin = Path(rule['origin'])
    local_file(origin, build)
    source = Path(values['CMAKE_HOME_DIRECTORY']).resolve()
    # Several pinned projects configure from a repository subdirectory
    # (zstd/build/cmake, expat/expat). The caller verifies the whole locked Git
    # tree; compiler inputs elsewhere in that same tree remain legitimate.
    git = subprocess.run(['git', '-C', str(source), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    if git.returncode == 0:
        root = Path(git.stdout.strip()).resolve()
        require(source.is_relative_to(root), 'Configured source escapes its Git tree')
        source = root
    proof = target_proof(build, origin, source, rule['kind'])
    proof['install_rule'] = rule
    return origin, proof
