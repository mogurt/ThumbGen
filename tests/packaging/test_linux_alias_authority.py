"""Real native fixtures for SONAME aliases and non-suffix library authority."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

if __name__ != '__main__':
    raise unittest.SkipTest('Explicit Linux native fixture run required: --work NEW_DIRECTORY')

parser = argparse.ArgumentParser()
parser.add_argument('--work', required=True, type=Path)
args, remaining = parser.parse_known_args()
repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('overlays', repo / 'scripts/linux/runtime_overlays.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
args.work.mkdir(parents=True, exist_ok=False)
commands = []
def run(command):
    process = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    number = len(commands)
    (args.work / ('command-%d.stdout' % number)).write_text(process.stdout)
    (args.work / ('command-%d.stderr' % number)).write_text(process.stderr)
    commands.append(dict(command=list(map(str, command)), exit=process.returncode))
    (args.work / 'commands.json').write_text(json.dumps(commands, indent=2))
    if process.returncode: raise RuntimeError(process.stderr)
    return process.stdout.strip()


class AliasAuthority(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.projects=[]
        for generator in ('Ninja', 'Unix Makefiles'):
            root=args.work/generator.replace(' ', '-')
            source,build,prefix=(root/n for n in ('source','build','sdk'))
            source.mkdir(parents=True)
            (source/'unit.c').write_text('int authored_native_fixture(void) { return 6; }\n')
            (source/'tool.c').write_text('int main(void) { return 0; }\n')
            (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(alias_fixture C)\nadd_library(official SHARED unit.c)\nset_target_properties(official PROPERTIES VERSION 1.2 SOVERSION 1)\nadd_library(known_static STATIC unit.c)\nadd_library(neutral_static STATIC unit.c)\nset_target_properties(neutral_static PROPERTIES SUFFIX ".bin")\nadd_executable(helper tool.c)\ninstall(TARGETS official known_static neutral_static helper LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)\ninstall(PROGRAMS $<TARGET_FILE:helper> DESTINATION bin RENAME renamed-helper)\n')
            run(['cmake','-S',source,'-B',build,'-G',generator,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+str(prefix)])
            run(['cmake','--build',build,'--parallel','2'])
            run(['cmake','--install',build])
            rogue=root/'rogue.c';rogue.write_text('int authored_native_fixture(void) { return 99; }\n')
            run(['cc','-shared','-fPIC',rogue,'-Wl,-soname,libofficial.so.1','-o',root/'rogue.bin'])
            cls.projects.append((root,build,prefix))

    def candidate(self, project):
        root,build,prefix=project
        target=root/self.id().split('.')[-1]
        shutil.copytree(prefix,target,symlinks=True)
        return target

    def audit(self, build, prefix):
        # Candidate map is deliberately regenerated after every mutation.
        receipt={p.relative_to(prefix).as_posix():m.sha(p) for p in prefix.rglob('*') if p.is_file()}
        return m.audit_installations(prefix,[(build,prefix)],receipt)

    def consumer(self, root, prefix, name):
        source=root/'consumer.c'
        source.write_text('#include <stdio.h>\nint authored_native_fixture(void);\nint main(void) { printf("%d\\n", authored_native_fixture()); return 0; }\n')
        exe=root/name
        run(['cc',source,'-L'+str(prefix/'lib'),'-lofficial','-Wl,-rpath,'+str(prefix/'lib'),'-o',exe])
        return exe

    def test_normal_and_install_prefix_relocation(self):
        for project in self.projects:
            root,build,prefix=project
            original=self.audit(build,prefix)
            moved=self.candidate(project)
            relocated=self.audit(build,moved)
            self.assertEqual((len(original['products']),len(relocated['products'])),(3,3))
            self.assertEqual(m.artifact_kind(moved/'lib/libneutral_static.bin'),'archive')
            m.authorize_library(moved/'lib/libneutral_static.bin',relocated['verified_files'])
            self.assertEqual((len(original['tools']),len(relocated['tools'])),(2,2))
            self.assertEqual(run([self.consumer(root,moved,'relocated-consumer')]),'6')

    def test_legal_internal_alias_chain(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            alias=prefix/'lib/libofficial.so.1';alias.unlink();alias.symlink_to('neutral-hop')
            (prefix/'lib/neutral-hop').symlink_to('libofficial.so.1.2')
            proof=self.audit(build,prefix)
            self.assertIn(str(alias),proof['aliases'])
            self.assertEqual(run([self.consumer(root,prefix,'chain-consumer')]),'6')

    def test_redirect_changes_consumer_but_audit_refuses(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            before=self.audit(build,prefix)
            exe=self.consumer(root,prefix,'changed-consumer');self.assertEqual(run([exe]),'6')
            payload=prefix/'lib/payload.bin';shutil.copyfile(root/'rogue.bin',payload)
            alias=prefix/'lib/libofficial.so.1';alias.unlink();alias.symlink_to('payload.bin')
            self.assertEqual(run([exe]),'99')
            with self.assertRaisesRegex(ValueError,'alias does not resolve to its verified native product'):
                self.audit(build,prefix)
            with self.assertRaisesRegex(ValueError,'not a verified native library'):
                m.authorize_library(payload,before['verified_files'])

    def test_alias_chain_to_unverified_payload(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            shutil.copyfile(root/'rogue.bin',prefix/'lib/payload.bin')
            alias=prefix/'lib/libofficial.so.1';alias.unlink();alias.symlink_to('hop')
            (prefix/'lib/hop').symlink_to('payload.bin')
            with self.assertRaisesRegex(ValueError,'verified native product'):self.audit(build,prefix)

    def test_prefix_escape_even_to_identical_bytes(self):
        for project in self.projects:
            root,build,original=project;prefix=self.candidate(project)
            alias=prefix/'lib/libofficial.so.1';alias.unlink();alias.symlink_to(original/'lib/libofficial.so.1.2')
            with self.assertRaisesRegex(ValueError,'escapes selected prefix'):self.audit(build,prefix)

    def test_cycle(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            alias=prefix/'lib/libofficial.so.1';alias.unlink();alias.symlink_to('hop')
            (prefix/'lib/hop').symlink_to('libofficial.so.1')
            with self.assertRaisesRegex(ValueError,'alias cycle'):self.audit(build,prefix)

    def test_unknown_elf_without_library_suffix(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            shutil.copyfile(root/'rogue.bin',prefix/'ordinary-data')
            with self.assertRaisesRegex(ValueError,'Unverified installed ELF/archive by content'):self.audit(build,prefix)

    def test_identical_elf_copy_is_not_a_new_authorized_path(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            shutil.copyfile(prefix/'lib/libofficial.so.1.2',prefix/'identical-payload.bin')
            with self.assertRaisesRegex(ValueError,'Unverified installed ELF/archive by content'):self.audit(build,prefix)

    def test_unknown_archive_without_archive_suffix(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            shutil.copyfile(prefix/'lib/libknown_static.a',prefix/'data.bin')
            with self.assertRaisesRegex(ValueError,'Unverified installed ELF/archive by content'):self.audit(build,prefix)

    def test_verified_tool_is_not_runtime_library_authority(self):
        for root,build,prefix in self.projects:
            proof=self.audit(build,prefix)
            with self.assertRaisesRegex(ValueError,'not a verified native library'):
                m.authorize_library(prefix/'bin/helper',proof['verified_files'])

    def test_stale_library_digest_refuses_change(self):
        for project in self.projects:
            root,build,_=project;prefix=self.candidate(project)
            proof=self.audit(build,prefix)
            real=prefix/'lib/libofficial.so.1.2';real.write_bytes((root/'rogue.bin').read_bytes())
            with self.assertRaisesRegex(ValueError,'not a verified native library'):
                m.authorize_library(real,proof['verified_files'])


if __name__=='__main__':unittest.main(argv=[sys.argv[0],*remaining])
