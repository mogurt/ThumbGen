"""Content and alias inventory for an already verified native installation.

Receipt maps never add authority. Each alias is resolved within the selected
prefix and must end at an explicitly verified install product.
"""
import hashlib
import os
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def artifact_kind(path):
    path = Path(path)
    if not path.is_file():
        return None
    with path.open('rb') as stream:
        header = stream.read(8)
    if header.startswith(b'\x7fELF'):
        return 'elf'
    if header == b'!<arch>\n':
        return 'archive'
    return None


def resolve_inside(path, prefix):
    """Resolve every hop, rejecting cycles, broken links and prefix escapes."""
    original = Path(prefix).absolute()
    prefix = original.resolve(strict=True)
    path = Path(path).absolute()
    require(path.is_relative_to(original), 'Alias path escapes selected prefix: ' + str(path))
    require('..' not in path.relative_to(original).parts, 'Alias path escapes selected prefix: ' + str(path))
    pending = list(path.relative_to(original).parts)
    current, visited, chain = prefix, set(), []
    while pending:
        part = pending.pop(0)
        candidate = current / part
        if candidate.is_symlink():
            require(str(candidate) not in visited, 'Installed alias cycle: ' + str(candidate))
            visited.add(str(candidate))
            link = os.readlink(candidate)
            target = Path(link) if Path(link).is_absolute() else candidate.parent / link
            target = Path(os.path.abspath(target))  # lexical; do not follow a hidden hop
            require(target.is_relative_to(prefix), 'Installed alias escapes selected prefix: ' + str(candidate))
            chain.append(dict(path=str(candidate), target=link))
            pending = list(target.relative_to(prefix).parts) + pending
            current = prefix
        else:
            require(candidate.exists(), 'Broken installed alias or missing product: ' + str(candidate))
            current = candidate
    require(current.is_relative_to(prefix), 'Installed alias escapes selected prefix')
    return current, chain


def audit_prefix(prefix, library_files, tool_files, declared_aliases):
    """Verify aliases first, then inspect all files by content, never suffix."""
    prefix = Path(prefix).resolve(strict=True)
    libraries, tools = dict(library_files), dict(tool_files)
    all_files = {**libraries, **tools}
    aliases = {}
    for name, expected in declared_aliases.items():
        target, chain = resolve_inside(Path(name), prefix)
        require(str(target) == expected and expected in all_files,
                'Installed alias does not resolve to its verified native product: ' + name)
        require(sha(target) == all_files[expected], 'Installed alias product digest changed: ' + name)
        aliases[name] = dict(target=str(target), sha256=all_files[expected], chain=chain, declared=True)
    for path in sorted(prefix.rglob('*')):
        if path.is_symlink():
            target, chain = resolve_inside(path, prefix)
            kind = artifact_kind(target)
            if kind:
                require(str(target) in all_files and sha(target) == all_files[str(target)],
                        'Installed alias targets an unverified native product: ' + str(path))
                aliases.setdefault(str(path), dict(target=str(target), sha256=all_files[str(target)], chain=chain, declared=False))
        elif path.is_file() and artifact_kind(path):
            require(str(path) in all_files and sha(path) == all_files[str(path)],
                    'Unverified installed ELF/archive by content: ' + str(path))
    return aliases


def authorize_library(path, verified_files):
    path = Path(path).resolve(strict=True)
    require(str(path) in verified_files and sha(path) == verified_files[str(path)],
            'Runtime/link dependency is not a verified native library: ' + str(path))
    return verified_files[str(path)]
