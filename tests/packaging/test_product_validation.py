import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('validation', ROOT / 'scripts/windows/product_validation.py')
validation = importlib.util.module_from_spec(spec); spec.loader.exec_module(validation)


class ExecutionEvidence(unittest.TestCase):
    def setUp(self):
        from PIL import Image
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve(); binary = self.root / 'bin'; binary.mkdir()
        for name in ('thumbgen.exe', 'lcms2.dll'):
            (binary / name).write_bytes(name.encode())
        self.exe = binary / 'thumbgen.exe'
        self.closure = dict(exe_sha256=validation.sha(self.exe), overlays=[{'kind': 'bounded'}], files=[dict(name=p.name, sha256=validation.sha(p)) for p in binary.iterdir()])
        recipe = self.root / 'trusted-validator.py'; recipe.write_bytes(b'trusted validator fixture\n')
        reference = self.root / 'color/report.json'; reference.parent.mkdir(); reference.write_text('{}')
        self.identity = {'fixture': dict(path=str(recipe), sha256=validation.sha(recipe), lf_sha256=validation.sha(recipe))}
        self.report = dict(schema_version=2, kind='independent-windows-product-validation', passed=True,
            fixtures=dict(color_evidence=str(reference.parent), deep_evidence=str(self.root)),
            exe_sha256=validation.sha(self.exe), tested_exe=str(self.exe), validator=self.identity,
            tested_runtime={p.name: dict(path=str(p), sha256=validation.sha(p)) for p in binary.iterdir()},
            loaded_modules={str(p): validation.sha(p) for p in binary.iterdir()}, cases=[])
        for name, layout in validation.CASES.items():
            stdout, stderr = self.root / (name + '.stdout'), self.root / (name + '.stderr')
            stderr.write_bytes(b'')
            case = dict(name=name, exit=0, command=[str(self.exe), 'capabilities' if layout is None else 'convert'])
            row = dict(type='capabilities', deep_compressions=['zstd'])
            if layout:
                target = self.root / (name + '.output.png'); Image.new('RGB', layout[:2]).save(target)
                case.update(output=self.entry(target), pixels=layout[0] * layout[1], max_lsb=0)
                case['command'] += ['input', '--output', str(target), '--json']
                row = dict(type='result', ok=True, outputs=[dict(path=str(target), status='success')])
            stdout.write_text(json.dumps(row) + '\n')
            case.update(stdout=self.entry(stdout), stderr=self.entry(stderr)); self.report['cases'].append(case)
        self.seal()
        self.identity_patch = patch.object(validation, 'validator_identity', return_value=copy.deepcopy(self.identity))
        self.identity_patch.start(); self.addCleanup(self.identity_patch.stop)

    def entry(self, path):
        return dict(path=str(path), sha256=validation.sha(path))

    def seal(self):
        commands, modules = self.root / 'commands.json', self.root / 'modules.json'
        commands.write_text(json.dumps(self.report['cases'])); modules.write_text(json.dumps(self.report['loaded_modules']))
        self.report['command_index'], self.report['module_index'] = self.entry(commands), self.entry(modules)
        self.report['artifacts'] = [self.entry(p) for p in self.root.rglob('*') if p.is_file()]

    def check(self):
        return validation.check_report(self.report, self.closure)

    def test_complete_evidence(self):
        self.assertEqual(self.check()['cases'], 9)

    def test_optional_unpatched_core_requires_all_seven_color_cases(self):
        self.closure['overlays'] = []
        self.report['cases'] = [c for c in self.report['cases'] if not c['name'].startswith('deep-')]
        case = self.report['cases'][0]; path = Path(case['stdout']['path'])
        path.write_text(json.dumps(dict(type='capabilities', deep_compressions=[])))
        case['stdout'] = self.entry(path); self.seal()
        self.assertEqual(self.check()['pixels'], 384)

    def test_four_field_boolean_only_report_rejected(self):
        with self.assertRaisesRegex(ValueError, 'complete independent'):
            validation.check_report(dict(kind=self.report['kind'], exe_sha256=self.report['exe_sha256'], passed=True, artifacts=[]), self.closure)

    def test_empty_artifacts_rejected(self):
        self.report['artifacts'] = []
        with self.assertRaisesRegex(ValueError, 'artifact coverage'):
            self.check()

    def test_missing_case_rejected_even_with_consistent_command_index(self):
        self.report['cases'].pop(); self.seal()
        with self.assertRaisesRegex(ValueError, 'case coverage'):
            self.check()

    def test_missing_output_coverage_rejected(self):
        path = self.report['cases'][1]['output']['path']
        self.report['artifacts'] = [e for e in self.report['artifacts'] if e['path'] != path]
        with self.assertRaisesRegex(ValueError, 'artifact coverage'):
            self.check()

    def test_missing_output_file_rejected(self):
        Path(self.report['cases'][1]['output']['path']).unlink()
        with self.assertRaises(OSError):
            self.check()

    def test_missing_loaded_module_rejected_even_with_consistent_index(self):
        self.report['loaded_modules'].pop(str(self.exe.parent / 'lcms2.dll')); self.seal()
        with self.assertRaisesRegex(ValueError, 'loaded runtime coverage'):
            self.check()

    def test_module_substitution_rejected(self):
        original = self.exe.parent / 'lcms2.dll'
        substitute = self.root / 'replacement/lcms2.dll'; substitute.parent.mkdir(); substitute.write_bytes(original.read_bytes())
        self.report['loaded_modules'].pop(str(original)); self.report['loaded_modules'][str(substitute)] = validation.sha(substitute); self.seal()
        with self.assertRaisesRegex(ValueError, 'substituted'):
            self.check()

    def test_module_digest_mismatch_rejected(self):
        self.report['loaded_modules'][str(self.exe)] = '0' * 64; self.seal()
        with self.assertRaisesRegex(ValueError, 'module digest'):
            self.check()

    def test_changed_tested_dll_rejected_against_closure(self):
        dll = self.exe.parent / 'lcms2.dll'; dll.write_bytes(b'replacement')
        self.report['tested_runtime']['lcms2.dll'] = self.entry(dll)
        self.report['loaded_modules'][str(dll)] = validation.sha(dll); self.seal()
        with self.assertRaisesRegex(ValueError, 'runtime path/digest'):
            self.check()

    def test_untrusted_validator_content_rejected(self):
        self.report['validator']['fixture']['lf_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'recipe content'):
            self.check()

    def test_original_forged_frozen_evidence_requires_refreeze(self):
        with self.assertRaisesRegex(ValueError, 'validation mode'):
            validation.check_frozen({'kind': 'thumbgen-tested-windows-product'}, self.closure)

    def test_overlay_candidate_cannot_downgrade_to_ctest_count(self):
        with self.assertRaisesRegex(ValueError, 'validation mode'):
            validation.check_frozen(dict(validation_mode='full-ctest', ctest=dict(passed=116, names=list(map(str, range(116))))), self.closure)
