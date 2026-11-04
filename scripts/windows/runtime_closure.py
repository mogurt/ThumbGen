"""Audit x64 PE import closure against the locked Windows SDK (no PATH search).

Normal and delay imports are included. LoadLibrary/plugin paths and symbol export
forwarders are not a complete runtime proof; use a clean package execution too.
PE layout: https://learn.microsoft.com/windows/win32/debug/pe-format
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def imports(path):
    """Return declared imports, rejecting malformed/non-x64 PE input."""
    data = path.read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError('Truncated PE: ' + str(path))
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('Expected PE image: ' + str(path))
    pe, = unpack('<I', 0x3c)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Expected PE signature: ' + str(path))
    machine, count = unpack('<HH', pe + 4)
    optional_size, = unpack('<H', pe + 20)
    optional = pe + 24
    magic, = unpack('<H', optional)
    if machine != 0x8664 or magic != 0x20b or optional_size < 112:
        raise ValueError('Expected AMD64 PE32+: ' + str(path))
    directory_count, = unpack('<I', optional + 108)
    if directory_count > (optional_size - 112) // 8:
        raise ValueError('Invalid PE directory count: ' + str(path))
    sections = []
    for index in range(count):
        # VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData.
        sections.append(unpack('<4I', optional + optional_size + index * 40 + 8))

    def rva_offset(rva, size=1):
        for virtual_size, address, raw_size, raw in sections:
            if address <= rva < address + max(virtual_size, raw_size):
                delta = rva - address
                if delta + size <= raw_size and raw + delta + size <= len(data):
                    return raw + delta
                break
        raise ValueError('Unmapped PE RVA: ' + str(path))

    def dll_name(rva):
        offset = rva_offset(rva)
        end = data.find(b'\0', offset, min(len(data), offset + 261))
        if end < 0:
            raise ValueError('Unterminated import name: ' + str(path))
        # Recheck the whole string fits in its section's raw data.
        rva_offset(rva, end - offset + 1)
        name = data[offset:end].decode('ascii')
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+\.dll', name, re.IGNORECASE):
            raise ValueError('Unsafe/unsupported imported DLL: ' + repr(name))
        return name

    result = []
    for directory, width, label in ((1, 20, 'import'), (13, 32, 'delay_import')):
        if directory >= directory_count:
            continue
        rva, size = unpack('<II', optional + 112 + directory * 8)
        if not rva and not size:
            continue
        if not rva or size < width:
            raise ValueError('Invalid import directory: ' + str(path))
        terminated = False
        for offset in range(0, size - width + 1, width):
            values = unpack('<' + 'I' * (width // 4), rva_offset(rva + offset, width))
            if not any(values):
                terminated = True
                break
            if directory == 13:
                if values[0] != 1:
                    raise ValueError('Unsupported VA/reserved delay descriptor: ' + str(path))
                name_rva = values[1]
            else:
                name_rva = values[3]
            result.append(dict(name=dll_name(name_rva), kind=label))
        if not terminated:
            raise ValueError('Unterminated import directory: ' + str(path))
    return result


def audit(exe, sdk, system):
    repo = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('thumbgen_verify_sdk', repo / 'scripts/verify-sdk.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    sdk_receipt = verifier.verify(sdk, repo / 'dependencies/windows-sdk.explicit.txt', [])
    owners, packages = {}, {}
    for path in sorted((sdk / 'conda-meta').glob('*.json')):
        record = json.loads(path.read_text(encoding='utf-8'))
        packages[record['name']] = record
        for item in record.get('paths_data', {}).get('paths', []):
            relative = item['_path']
            if relative.lower().endswith('.dll'):
                key = relative.replace('\\', '/').lower()
                if key in owners:
                    raise ValueError('Multiple DLL owners: ' + relative)
                owners[key] = record, item
    # Fixed SDK locations only, never the calling shell's PATH/current directory.
    available = {}
    for directory in (sdk / 'Library/bin', sdk):
        for path in sorted(directory.glob('*.dll')):
            name = path.name.lower()
            if name in available and digest(available[name]) != digest(path):
                raise ValueError('Ambiguous SDK DLL: ' + path.name)
            available.setdefault(name, path)
    queue, resolved, external = [exe], {}, {}
    used_packages = {'nlohmann_json'}  # Header-only direct product dependency.
    while queue:
        path = queue.pop(0)
        key = path.name.lower()
        if key in resolved:
            continue
        checksum = digest(path)
        record = dict(name=path.name, sha256=checksum, size=path.stat().st_size, imports=imports(path))
        if path != exe:
            owner, entry = owners[path.relative_to(sdk).as_posix().lower()]
            expected = entry.get('sha256_in_prefix') or entry.get('sha256')
            if not expected or checksum != expected:
                raise ValueError('Installed DLL contents differ from package metadata: ' + str(path))
            record['package'] = owner['name']
            used_packages.add(owner['name'])
        resolved[key] = record
        for dependency in record['imports']:
            name = dependency['name'].lower()
            if name.startswith(('api-ms-', 'ext-ms-')):
                dependency['resolution'] = 'windows_api_contract'
                external[name] = dict(name=name, kind='api_contract', minimum_os_verified=False)
            elif name == 'ucrtbase.dll':
                if not (system / name).is_file():
                    raise ValueError('Target-system UCRT unavailable')
                dependency['resolution'] = 'windows_system'
                external[name] = dict(name=name, kind='system32', minimum_os_verified=False)
            elif name in available:
                dependency['resolution'] = 'bundled'
                queue.append(available[name])
            elif re.match(r'(?:msvcp|vcruntime|concrt)\d', name):
                raise ValueError('VC runtime must come from locked SDK: ' + name)
            elif (system / name).is_file():
                dependency['resolution'] = 'windows_system'
                external[name] = dict(name=name, kind='system32', minimum_os_verified=False)
            else:
                raise ValueError('Unresolved DLL: ' + path.name + ' -> ' + name)
    components = []
    for name in sorted(used_packages):
        package = packages[name]
        components.append({key: package.get(key) for key in ('name', 'version', 'build', 'license', 'url', 'sha256')})
    return dict(schema_version=1, exe_sha256=digest(exe), sdk_lock_sha256=sdk_receipt['lock_sha256'],
                recipe_sha256=digest(Path(__file__).resolve()), architecture='AMD64',
                files=[resolved[key] for key in sorted(resolved)],
                system_dependencies=[external[key] for key in sorted(external)], components=components,
                redistribution_ready=False, minimum_platform_verified=False,
                limitations=['Declared normal/delay imports only; runtime LoadLibrary/plugins and export forwarders require separate execution validation.',
                             'System32 availability is checked on the current development host, not the minimum target OS.',
                             'Package license metadata is an index, not complete license/notice or corresponding-source compliance.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--system-dir', type=Path, default=Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = audit(args.exe.resolve(), args.sdk.resolve(), args.system_dir.resolve())
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, str(error) + '\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(files=len(report['files']), components=len(report['components']),
                          system_dependencies=len(report['system_dependencies']), redistribution_ready=False)))
