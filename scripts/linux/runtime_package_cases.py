"""Full stored-pixel package regressions using authored, package-external inputs."""
from decimal import Decimal
import hashlib
import importlib.util
import json
import math
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_cms_reference(reference):
    lock = json.loads((Path(__file__).resolve().parents[2] / 'tests/packaging/linux-cms-reference-v1.json').read_text())
    golden = json.loads((reference / 'report.json').read_text())
    require(isinstance(golden, dict), 'CMS reference report must be an object')
    rows = golden.get('cases')
    require(isinstance(rows, list) and all(isinstance(row, dict) for row in rows) and len(rows) == len(lock['cases']) and
            {row.get('name') for row in rows} == set(lock['cases']), 'CMS reference case set differs from authored coverage')
    for row in rows:
        identity = lock['cases'][row['name']]
        image = reference / row['name']
        require(image.is_file() and row.get('source_sha256') == identity['input_sha256'] and sha(image) == identity['input_sha256'],
                'CMS reference input identity differs: ' + row['name'])
        expected = row.get('expected')
        require(isinstance(expected, list) and len(expected) == identity['width'] * identity['height'] and
                all(isinstance(pixel, list) and len(pixel) == identity['bands'] and
                    all(type(v) is int and 0 <= v <= 255 for v in pixel) for pixel in expected),
                'CMS reference pixel dimensions differ: ' + row['name'])
        encoded = json.dumps(expected, separators=(',', ':')).encode()
        require(hashlib.sha256(encoded).hexdigest() == identity['expected_sha256'], 'CMS formula expectation identity differs: ' + row['name'])
    config = reference / 'project.ocio'
    require(config.is_file() and sha(config) == lock['project_config_sha256'], 'CMS project config identity differs')
    return rows, lock


def exercise(exe, output, source, fixtures, cms_reference, deep_fixtures, command, cms_native=None):
    spec = importlib.util.spec_from_file_location('package_pixel_oracle', source / 'tests/qa/alpha_reference.py')
    oracle = importlib.util.module_from_spec(spec); spec.loader.exec_module(oracle)
    directory = output / 'regressions'; directory.mkdir()
    report = dict(oracle_sha256=sha(source / 'tests/qa/alpha_reference.py'), cms=[], deep=[], alpha=[], batch=None)
    def save():
        (directory / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    rows, cms_lock = validate_cms_reference(cms_reference)
    report['cms_reference_sha256'] = sha(cms_reference / 'report.json')
    for row in rows:
        image = cms_reference / row['name']
        assert sha(image) == row['source_sha256']
        target = directory / (row['name']+'.png')
        values = [str(exe), 'convert', str(image), '--output', str(target), '--size', '8', '--preset', 'fast',
                  '--threads', '1', '--ocio-config', str(cms_reference / 'project.ocio') if row['name'] == 'project.png' else 'ocio://default']
        if row['name'] in ('rgba.png', 'gray-alpha.png', 'associated.tif'):
            values += ['--background', 'transparent']
        if row['name'] == 'bypass.png':
            values += ['--input-space', 'sRGB']
        values += ['--json']
        records = [json.loads(line) for line in command(*values).splitlines() if line.strip()]
        w,h,bands,pixels = oracle.read_png(target)
        expected = row['expected']
        identity = cms_lock['cases'][row['name']]
        require((w,h,bands,len(pixels)) == (identity['width'], identity['height'], identity['bands'], len(expected)), 'CMS actual output dimensions differ')
        error = max(abs(a-b) for p,e in zip(pixels,expected) for a,b in zip(p,e))
        report['cms'].append(dict(name=row['name'], width=w, height=h, bands=bands, source_sha256=sha(image), output_sha256=sha(target),
            expected=expected, actual=pixels, max_lsb=error, records=records))
        save(); require(error <= 2 and sha(image) == row['source_sha256'], 'CMS pixel comparison or unchanged input check failed')
    def srgb(v):
        return round(255*(12.92*v if v <= .0031308 else 1.055*v**(1/2.4)-.055))
    deep_sources=[]
    for layout in ('scan','tile'):
        image=deep_fixtures/(layout+'.exr'); deep_sources.append(image)
        before=sha(image); target=directory/(layout+'.png')
        records=[json.loads(line) for line in command(str(exe),'convert',str(image),'--output',str(target),
            '--deep-mode','flatten','--threads','1','--input-space','linear','--ocio-config','ocio://default',
            '--size','129x9','--preset','fast','--background','black','--json').splitlines()]
        w,h,bands,pixels=oracle.read_png(target)
        assert (w,h,bands)==(129,9,3)
        expected=[]
        for y in range(9):
            pixel=(0,srgb(.75),srgb(.5)) if y%4==1 else (srgb(.5),srgb(.8125),srgb(.5)) if y%4==2 else (0,0,0)
            expected.extend([pixel]*129)
        error=max(abs(a-b) for p,e in zip(pixels,expected) for a,b in zip(p,e))
        report['deep'].append(dict(layout=layout, input_sha256=before, output_sha256=sha(target), max_lsb=error,
                                   actual=pixels,expected=expected,records=records))
        save(); assert error<=1 and sha(image)==before
    impulse=fixtures/'lanczos-alpha.exr'; before=sha(impulse)
    pixels=[(Decimal(0),)*3+(Decimal(int(x==3)),) for y in range(2) for x in range(8)]
    fixture=dict(data=(0,0,8,2),display=(0,0,8,2),pixels=pixels)
    for background in ('white','black','transparent'):
        target=directory/('lanczos-'+background+'.png')
        records=[json.loads(line) for line in command(str(exe),'convert',str(impulse),'--output',str(target),
            '--preset','quality','--input-space','linear','--ocio-config','ocio://default','--background',background,
            '--size','4x1','--threads','1','--json').splitlines()]
        expected=oracle.reference(fixture,2,'quality',background)
        metrics=oracle.compare(target,expected)
        report['alpha'].append(dict(background=background, input_sha256=before, metrics=metrics,
            expected=[[str(v) for v in p] for p in expected[2]], actual=oracle.read_png(target)[3],records=records))
        save(); assert metrics['passed'] and sha(impulse)==before
    jobs=[dict(id=str(i),sequence_id='package-zstd',input=str(deep_sources[i%2]),
        outputs=[dict(path=str(directory/('batch-%d.png'%i)))],
        options=dict(deep_samples_per_pixel=1) if i==1 else {}) for i in range(3)]
    manifest=dict(schema_version=1,defaults=dict(deep_mode='flatten',ocio_config='ocio://default',input_space='linear',
        threads=1,size=[129,9],preset='fast'),jobs=jobs)
    path=directory/'jobs.json'; path.write_text(json.dumps(manifest))
    rows=[json.loads(line) for line in command(str(exe),'batch','--manifest',str(path),'--json',expected_exit=3).splitlines()]
    results=[r for r in rows if r['type']=='result']
    assert [r['status'] for r in results]==['success','failed','success']
    assert results[1]['error']['code']=='deep_sample_limit_exceeded' and results[2]['selection']['lock_status']=='reused'
    assert len({json.dumps(r['selection']['locked'],sort_keys=True) for r in results})==1
    assert oracle.read_png(directory/'batch-0.png')==oracle.read_png(directory/'batch-2.png')
    report['batch']=rows
    if cms_native:
        report['native_cms']=dict(executable_sha256=sha(cms_native), stdout=command(str(cms_native)))
    report['scope']='Authored CMS formula references, authored ZSTD stripe formulas, Decimal Lanczos oracle; package-external tools only'
    save()
    return dict(cms_cases=len(report['cms']),cms_pixels=sum(len(r['actual']) for r in report['cms']),
                deep_cases=len(report['deep']),deep_pixels=sum(len(r['actual']) for r in report['deep']),alpha_cases=len(report['alpha']),
                alpha_pixels=sum(len(r['actual']) for r in report['alpha']),batch_statuses=[r['status'] for r in results],native_cms=bool(cms_native),report=str(directory/'report.json'))
