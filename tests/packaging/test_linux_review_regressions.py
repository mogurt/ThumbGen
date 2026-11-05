"""Real native targets and the independent review's evidence-boundary probes."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--work', required=True, type=Path)
parser.add_argument('--reference', required=True, type=Path)
parser.add_argument('--archive', required=True, type=Path)
args, remaining = parser.parse_known_args()
repo = Path(__file__).resolve().parents[2]
def module(name):
    spec = importlib.util.spec_from_file_location(name, repo / ('scripts/linux/' + name + '.py'))
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result
m, cases = module('runtime_overlays'), module('runtime_package_cases')
args.work.mkdir(parents=True, exist_ok=False)
commands = []
def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    number = len(commands)
    (args.work / ('command-%d.stdout' % number)).write_text(result.stdout)
    (args.work / ('command-%d.stderr' % number)).write_text(result.stderr)
    commands.append(dict(command=list(map(str, command)), exit=result.returncode))
    (args.work / 'commands.json').write_text(json.dumps(commands, indent=2))
    if result.returncode: raise RuntimeError(result.stderr)


class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.projects = []
        for generator in ('Ninja', 'Unix Makefiles'):
            root = args.work / generator.replace(' ', '-')
            source, build, prefix = (root / n for n in ('source', 'build', 'prefix'))
            source.mkdir(parents=True)
            (source / 'unit.c').write_text('int authored_native_fixture(void) { return 6; }\n')
            (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(provenance C)\nadd_library(real STATIC unit.c)\nadd_library(shared SHARED unit.c)\nset_target_properties(shared PROPERTIES VERSION 1.2 SOVERSION 1)\ninstall(TARGETS real shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)\n')
            run(['cmake', '-S', source, '-B', build, '-G', generator, '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX='+str(prefix)])
            run(['cmake', '--build', build, '--parallel', '2'])
            run(['cmake', '--install', build])
            cls.projects.append((root, build, prefix))

    def installed(self, build, prefix, relative):
        return m.installed_product(build, prefix, relative, dict(installed_sha256={relative:m.sha(prefix/relative)}), {})

    def test_real_make_and_ninja_outputs(self):
        for root, build, prefix in self.projects:
            for relative in ('lib/libreal.a', 'lib/libshared.so.1.2'):
                proof = self.installed(build, prefix, relative)
                self.assertTrue(proof['native']['objects'] and proof['native']['sources'])

    def test_symlink_and_regular_same_name_decoys(self):
        for index, (root, build, prefix) in enumerate(self.projects):
            candidate = root / 'changed-prefix'; shutil.copytree(prefix, candidate, symlinks=True)
            relative = 'lib/libshared.so.1.2'
            file = candidate / relative; file.write_bytes(file.read_bytes()+b'UNBUILT')
            decoy = build / 'not-a-target'; decoy.mkdir()
            alias = decoy / file.name
            for symlink in (True, False):
                if symlink: alias.symlink_to(file)
                else: shutil.copyfile(file, alias)
                with self.assertRaisesRegex(ValueError, 'not the recorded build product'):
                    self.installed(build, candidate, relative)
                alias.unlink()

    def test_real_target_redirect_refused(self):
        for root, build, prefix in self.projects:
            relative = 'lib/libshared.so.1.2'
            origin, proof = m._native.installed_origin(build, relative, m.cache_values(build))
            original = origin.read_bytes(); origin.unlink(); origin.symlink_to(prefix / relative)
            try:
                with self.assertRaisesRegex(ValueError, 'symlink|escapes native build'):
                    self.installed(build, prefix, relative)
            finally:
                origin.unlink(); origin.write_bytes(original)

    def test_static_archive_object_mismatch(self):
        for root, build, prefix in self.projects:
            proof = self.installed(build, prefix, 'lib/libreal.a')
            obj = Path(next(iter(proof['native']['objects'])))
            original = obj.read_bytes(); obj.write_bytes(original+b'UNLINKED')
            try:
                with self.assertRaisesRegex(ValueError, 'archive object differs'):
                    self.installed(build, prefix, 'lib/libreal.a')
            finally: obj.write_bytes(original)

    def test_missing_native_install_rule(self):
        for root, build, prefix in self.projects:
            script = build / 'cmake_install.cmake'; original = script.read_bytes()
            script.write_text('# no target install rules\n')
            try:
                with self.assertRaisesRegex(ValueError, 'not one configured native install output'):
                    self.installed(build, prefix, 'lib/libreal.a')
            finally: script.write_bytes(original)


class Coverage(unittest.TestCase):
    def reference(self):
        target = args.work / self.id().split('.')[-1]
        shutil.copytree(args.reference, target)
        return target

    def mutate(self, edit, reason):
        target = self.reference(); file = target / 'report.json'
        report = json.loads(file.read_text()); edit(report)
        file.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, reason): cases.validate_cms_reference(target)

    def test_cms_positive(self):
        rows, lock = cases.validate_cms_reference(args.reference)
        self.assertEqual((len(rows), sum(len(r['expected']) for r in rows)), (11,704))

    def test_cms_empty(self): self.mutate(lambda r:r.update(cases=[]), 'case set differs')
    def test_cms_missing(self): self.mutate(lambda r:r['cases'].pop(), 'case set differs')
    def test_cms_duplicate(self): self.mutate(lambda r:r['cases'].__setitem__(0,r['cases'][1]), 'case set differs')
    def test_cms_wrong_case(self): self.mutate(lambda r:r['cases'][0].update(name='fake.png'), 'case set differs')
    def test_cms_changed_expectation(self): self.mutate(lambda r:r['cases'][0]['expected'][0].__setitem__(0,0), 'expectation identity differs')
    def test_cms_wrong_dimensions(self): self.mutate(lambda r:r['cases'][0]['expected'].pop(), 'pixel dimensions differ')
    def test_cms_changed_input(self):
        target=self.reference(); file=target/'rgb.png';file.write_bytes(file.read_bytes()+b'CHANGED')
        with self.assertRaisesRegex(ValueError,'input identity differs'):cases.validate_cms_reference(target)

    def test_component_positive_missing_and_changed_notices(self):
        with tarfile.open(args.archive) as archive:
            manifest=json.load(archive.extractfile(next(x for x in archive if x.name.endswith('/DEPENDENCIES.json'))))
        m.validate_component_inventory(manifest,repo)
        for changed in ('missing','no-notices','changed-notice'):
            value=json.loads(json.dumps(manifest))
            if changed=='missing':value['dependency_components']=[c for c in value['dependency_components'] if c['name']!='zstd']
            elif changed=='no-notices':value['dependency_components'][0]['notices']=[]
            else:value['dependency_components'][0]['notices'][0]['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'inventory differs'):m.validate_component_inventory(value,repo)


if __name__=='__main__': unittest.main(argv=[sys.argv[0],*remaining])
