"""Stage and audit frozen public Deep tests without editing shared scripts/tests."""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare_stage(cache):
    base = Path(__file__).with_name('stage-validation.py')
    text = base.read_text()
    before = "Path(test['command'][0]).name for test in suite['tests']\n    if Path(test['command'][0]).parent == build\n    and Path(test['command'][0]).name.startswith('thumbgen')"
    after = "Path(argument).name for test in suite['tests'] for argument in test['command']\n    if Path(argument).parent == build and Path(argument).name.startswith('thumbgen')"
    assert text.count(before) == 1
    text = text.replace(before, after)
    anchor = "for name in (*python_tests, 'requirements.txt'):"
    assert text.count(anchor) == 1
    text = text.replace(anchor, "if any(test['name'] == 'public_deep' for test in suite['tests']):\n    python_tests.append('test_deep_cli.py')\n    (stage / 'tests/qa').mkdir(exist_ok=True)\n    shutil.copy2(repo / 'tests/qa/alpha_reference.py', stage / 'tests/qa/alpha_reference.py')\n" + anchor)
    anchor = 'tests = []\n'
    assert text.count(anchor) == 1
    text = text.replace(anchor, "replacements[str(repo / 'tests/test_deep_cli.py')] = '@TESTS@/test_deep_cli.py'\n" + anchor)
    stage = Path(os.environ['THUMBGEN_VALIDATION_DIR'])
    artifact = stage / 'stage-validation-public-deep.py'
    artifact.write_text(text)
    (stage / 'stage-adapter.json').write_text(json.dumps(dict(base=str(base), base_sha256=digest(base),
        prepared_sha256=digest(artifact), collector_sha256=digest(__file__),
        changes=['Discover executables in every CTest argument', 'Copy public Deep and stdlib PNG oracle', 'Relocate new Python test']), indent=2) + '\n')
    sys.argv = [str(artifact), str(cache)]
    exec(compile(text, str(artifact), 'exec'), {'__file__': str(artifact), '__name__': '__staging_adapter__'})


def retain_public(stage, directory):
    directory.mkdir()
    records = []
    protocol = []
    emitted = []
    fixture_hashes = {}
    original_run = subprocess.run
    original_temp = tempfile.TemporaryDirectory

    class RetainedDirectory(original_temp):
        def __init__(self, *args, **kwargs):
            kwargs['dir'] = str(directory)
            super().__init__(*args, **kwargs)

        def cleanup(self):
            self._finalizer.detach()

        @classmethod
        def _cleanup(cls, *args, **kwargs):
            pass

    def observed_run(*args, **kwargs):
        value = original_run(*args, **kwargs)
        command = [str(item) for item in (args[0] if args else kwargs['args'])]
        records.append(dict(command=command, exit=value.returncode, stdout=value.stdout, stderr=value.stderr))
        if Path(command[0]).name == 'thumbgen_deep_cli_fixtures' and value.returncode == 0:
            fixture_hashes.update({name:dict(path=path,sha256=digest(path)) for name,path in json.loads(value.stdout).items()})
        if Path(command[0]).name == 'thumbgen':
            for line in value.stdout.splitlines():
                if not line.strip():
                    continue
                row=json.loads(line);protocol.append(row)
                for output in row.get('outputs', []):
                    if output.get('status')!='success' or output.get('format')!='png':
                        continue
                    original=Path(output['path']);copies=directory/'emitted-png';copies.mkdir(exist_ok=True)
                    copied=copies/('command-%03d-output-%03d.png'%(len(records),len(emitted)))
                    shutil.copyfile(original,copied)
                    assert digest(copied)==digest(original)
                    emitted.append(dict(command=len(records),id=row.get('id'),original=str(original),copy=str(copied),sha256=digest(copied)))
        return value

    script = stage / 'tests/test_deep_cli.py'
    sys.argv = [str(script), '--exe', str(stage / 'bin/thumbgen'), '--fixtures-exe', str(stage / 'bin/thumbgen_deep_cli_fixtures'),
                '--runtime-dir', str(stage / 'lib'), '--schema-dir', str(stage / 'schemas'), '-v']
    stdout, stderr = io.StringIO(), io.StringIO()
    tempfile.TemporaryDirectory, subprocess.run = RetainedDirectory, observed_run
    code = 0
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            try:
                runpy.run_path(str(script), run_name='__main__')
            except SystemExit as error:
                code = error.code or 0
    finally:
        tempfile.TemporaryDirectory, subprocess.run = original_temp, original_run
        (directory / 'stdout.log').write_text(stdout.getvalue())
        (directory / 'stderr.log').write_text(stderr.getvalue())
        (directory / 'exit-code.txt').write_text(str(code) + '\n')
        (directory / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        (directory / 'protocol.json').write_text(json.dumps(protocol, indent=2) + '\n')
        (directory / 'emitted-png.json').write_text(json.dumps(emitted, indent=2) + '\n')
    assert code == 0 and re.search(r'Ran 13 tests\b', stderr.getvalue())
    assert len(fixture_hashes) == 14
    for row in fixture_hashes.values():
        assert digest(row['path']) == row['sha256']
    # read_png was imported from the exact staged oracle by the unmodified suite.
    oracle = sys.modules['alpha_reference']
    pngs = []
    for path in sorted(directory.rglob('*.png')):
        width,height,bands,pixels = oracle.read_png(path)
        pngs.append(dict(path=str(path),sha256=digest(path),width=width,height=height,bands=bands,
                         decoded_sha256=hashlib.sha256(bytes(component for pixel in pixels for component in pixel)).hexdigest()))
    assert pngs
    summary = dict(cases=13,exit=code,fixtures=fixture_hashes,pngs=pngs,commands=len(records),emitted_png=emitted,protocol_records=len(protocol),
                   exe_sha256=digest(stage/'bin/thumbgen'),fixture_exe_sha256=digest(stage/'bin/thumbgen_deep_cli_fixtures'),
                   test_sha256=digest(script),oracle_sha256=digest(stage/'tests/qa/alpha_reference.py'),
                   scope='Unmodified public suite and PNG assertions; temp cleanup detached only to retain evidence; subprocess results unchanged')
    (directory/'retained-public.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(dict(cases=13,fixtures=14,pngs=len(pngs),commands=len(records))))


def audit():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('stage','source','mainline','official','previous-stage','retained-public'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--sdk',type=Path)
    args=parser.parse_args()
    result=args.stage/'results'/os.environ['WSL_DISTRO_NAME']
    load=lambda p:json.loads(p.read_text())
    suite=load(args.stage/'tests/ctest-suite.json')['tests'];inventory=load(result/'inventory.json')['tests']
    assert {t['name'] for t in suite}=={t['name'] for t in inventory}
    junit=ET.parse(result/'junit.xml').getroot();cases=junit.findall('.//testcase')
    assert len(cases)==len(suite) and not junit.findall('.//failure') and not junit.findall('.//error')
    skipped=len(junit.findall('.//skipped'))
    assert {'core_deep_selection','public_deep'}.issubset({t['name'] for t in suite})
    log=(result/'Testing/Temporary/LastTest.log').read_text()
    block=log.split('Test: public_deep\n',1)[1].split('<end of output>',1)[0]
    assert re.search(r'Ran 13 tests\b',block)
    qa=args.stage/'tests/qa/alpha_reference.py'
    assert digest(qa)==digest(args.source/'tests/qa/alpha_reference.py')
    manifest=result/'artifacts.sha256'
    with manifest.open('a') as stream:stream.write(digest(qa)+'  tests/qa/alpha_reference.py\n')
    common=[sys.executable,str(Path(__file__).with_name('audit-deep-output-validation.py'))]
    for key in ('stage','source','mainline','official','previous_stage'):
        common+=['--'+key.replace('_','-'),str(getattr(args,key))]
    common+=['--expected-count',str(len(suite))]
    if args.sdk:common+=['--sdk',str(args.sdk)]
    subprocess.run(common,check=True)
    retained=load(args.retained_public/'retained-public.json');assert retained['cases']==13 and retained['exit']==0
    assert retained['exe_sha256']==digest(args.stage/'bin/thumbgen') and len(retained['fixtures'])==14
    for row in retained['fixtures'].values():assert digest(row['path'])==row['sha256']
    for row in retained['pngs']:assert digest(row['path'])==row['sha256']
    from jsonschema import Draft202012Validator
    validator=Draft202012Validator(load(args.source/'schemas/protocol-v1.schema.json'))
    protocol=load(args.retained_public/'protocol.json')
    assert len(protocol)==retained['protocol_records']
    for row in protocol:validator.validate(row)
    for row in retained['emitted_png']:assert digest(row['copy'])==row['sha256']
    capabilities=load(result/'capabilities.json');assert capabilities['deep_modes']==['reject','flatten']
    summary=load(result/'deep-output-audit.json')
    summary.pop('internal_deep_reader_only',None)
    summary.update(public_deep_opt_in=True,default_deep_mode='reject',public_deep_cases=13,
        inventory_count=len(inventory),junit_count=len(cases),junit_skipped=skipped,junit_sha256=digest(result/'junit.xml'),
        retained_public=retained,retained_public_origin=str(args.retained_public),
        fixture_exe_sha256=digest(args.stage/'bin/thumbgen_deep_cli_fixtures'),
        native_selection_sha256=digest(args.stage/'bin/thumbgen_deep_selection_tests'),
        limits='Synthetic opt-in public Deep only; real assets, ZSTD, Inf, fault/cancel specialty, hard RSS, performance, physical platforms and release remain open')
    destination=result/'public-deep-audit.json'
    if destination.exists():raise FileExistsError(destination)
    destination.write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('artifacts','retained_public')},indent=2))


if __name__=='__main__':
    if sys.argv[1:2]==['--prepare-stage']:
        prepare_stage(Path(sys.argv[2]))
    elif sys.argv[1:2]==['--retain-public']:
        retain_public(Path(sys.argv[2]),Path(sys.argv[3]))
    else:
        audit()
