"""Inspect the loader-resolved transitive ELF closure; fail above GLIBC 2.17.

Run with the exact LD_LIBRARY_PATH intended for the tested executable/package.
GLIBCXX is reported, not assumed to be provided by the target system.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)

queue = [Path(p).resolve() for p in sys.argv[1:]]
if not queue:
    raise SystemExit('usage: audit-abi.py ELF [ELF ...]')
seen = set()
report = []
failures = []
while queue:
    path = queue.pop(0)
    if path in seen:
        continue
    seen.add(path)
    dynamic = run('readelf', '-d', str(path))
    versions = run('readelf', '--version-info', '--wide', str(path))
    # Only version requirements; definitions in glibc/libstdc++ are not requirements.
    needs = versions.split('Version needs section', 1)[-1] if 'Version needs section' in versions else ''
    required = sorted(set(re.findall(r'Name: ((?:GLIBCXX|GLIBC|CXXABI)_[A-Za-z0-9_.]+)', needs)))
    for version in required:
        if version.startswith('GLIBC_') and version != 'GLIBC_PRIVATE':
            number = version[6:]
            if not re.fullmatch(r'[0-9.]+', number) or tuple(map(int, number.split('.'))) > (2, 17):
                failures.append(str(path) + ': ' + version)
    loader = run('ldd', str(path)) if '(NEEDED)' in dynamic else ''
    if 'not found' in loader:
        failures.append(str(path) + ': unresolved dependency: ' + loader)
    for dependency in re.findall(r'(?:=>\s*)?(/[^\s()]+)', loader):
        resolved = Path(dependency).resolve()
        if resolved.is_file():
            queue.append(resolved)
    report.append(dict(path=str(path), required_versions=required,
                       needed=re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic),
                       rpath=re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]', dynamic), loader=loader))
print(json.dumps(dict(glibc_ceiling='2.17', objects=report, failures=failures), indent=2))
raise SystemExit(bool(failures))
