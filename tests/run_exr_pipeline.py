"""Give each native EXR regression invocation its own synthetic output tree."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='thumbgen-exr-pipeline-'))
result = subprocess.run([str(args.exe.resolve()), str(root / 'fixtures')])
if result.returncode:
    print(f'Preserved failing synthetic EXR fixtures: {root}', flush=True)
else:
    shutil.rmtree(root)
raise SystemExit(result.returncode)
