"""Relocate every discovered CTest command and retain matrix labels/timeouts."""
import json
from pathlib import Path
import sys

stage = Path(sys.argv[1]).resolve()
python = sys.argv[2]
suite = json.loads((stage / 'tests/ctest-suite.json').read_text())
mapping = {'@BIN@': str(stage / 'bin'), '@TESTS@': str(stage / 'tests'),
           '@SCHEMAS@': str(stage / 'schemas'),
           '@LIB@': str(stage / 'lib'), '@PYTHON@': python}
def quote(value):
    text = str(value)
    delimiter = '='
    while ']' + delimiter + ']' in text:
        delimiter += '='
    return '[' + delimiter + '[' + text + ']' + delimiter + ']'
def relocate(argument):
    for prefix, replacement in mapping.items():
        if argument == prefix or argument.startswith(prefix + '/'):
            return replacement + argument[len(prefix):]
    return argument
lines = []
for test in suite['tests']:
    name = quote(test['name'])
    command = ' '.join(quote(relocate(argument)) for argument in test['command'])
    lines.append(f'add_test({name} {command})')
    for key, value in test['properties'].items():
        if isinstance(value, list):
            value = ';'.join(value)
        lines.append(f'set_tests_properties({name} PROPERTIES {key} {quote(value)})')
Path('CTestTestfile.cmake').write_text('\n'.join(lines) + '\n')
print('Staged CTest cases:', len(suite['tests']))
