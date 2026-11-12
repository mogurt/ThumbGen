"""Static vendored components must be present in Linux runtime evidence."""
import copy,importlib.util,json,sys,unittest,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/linux'))
spec=importlib.util.spec_from_file_location('vendored_runtime_overlays',ROOT/'scripts/linux/runtime_overlays.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class VendoredComponents(unittest.TestCase):
 def manifest(self):
  inventory=json.loads((ROOT/'scripts/linux/runtime-notice-inventory.json').read_text())['components']
  components=[dict(name=name,**item) for name,item in inventory.items() if name!='OpenEXR-portable-DWA']
  lock=json.loads((ROOT/'dependencies/versions.json').read_text())
  for name,notice in [('skcms','skcms-BSD-3-Clause.txt'),('stb_image_resize','stb-image-resize-2.50.txt')]:
   components.append(dict(name=name,commit=lock[name]['revision'],notices=[dict(file=f'licenses/{name}/LICENSE',sha256=hashlib.sha256((ROOT/'dependencies/notices'/notice).read_bytes()).hexdigest())]))
  return dict(dependency_components=components)
 def test_complete(self):module.validate_component_inventory(self.manifest(),ROOT)
 def test_missing_static_component(self):
  for name in ('skcms','stb_image_resize'):
   manifest=self.manifest();manifest['dependency_components']=[c for c in manifest['dependency_components'] if c['name']!=name]
   with self.assertRaisesRegex(ValueError,'inventory differs'):module.validate_component_inventory(manifest,ROOT)
 def test_changed_license(self):
  manifest=self.manifest();next(c for c in manifest['dependency_components'] if c['name']=='skcms')['notices'][0]['sha256']='0'*64
  with self.assertRaisesRegex(ValueError,'notice inventory differs'):module.validate_component_inventory(manifest,ROOT)
if __name__=='__main__':unittest.main()
