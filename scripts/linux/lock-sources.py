"""Record source pins from the verified OIIO version, without editing that tree."""
import json
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[2]
packages = ['ZLIB', 'Imath', 'OpenEXR', 'libjpeg-turbo', 'TIFF', 'PNG', 'WebP',
            'OpenColorIO', 'fmt', 'Robinmap', 'libdeflate', 'zstd', 'pystring',
            'expat', 'yaml-cpp', 'minizip-ng']
records = []
for name in packages:
    path = repo / '.deps/oiio-src/src/cmake' / ('build_' + name + '.cmake')
    data = path.read_text()
    def capture(key):
        match = re.search(r'\b' + re.escape(name + '_') + r'(?:BUILD_)?' + re.escape(key) + r'\s+"?([^"\s)]+)', data)
        if not match:
            raise ValueError(name + ': missing ' + key)
        return match[1]
    records.append(dict(package=name, version=capture('BUILD_VERSION'),
                        repository=capture('GIT_REPOSITORY'), commit=capture('GIT_COMMIT')))
print(json.dumps(dict(oiio_tag='v3.2.1.2', oiio_commit='5c4408f545f1da5f30102c5f1c7151dbd9faf522',
                      dependencies=records, nlohmann_json=dict(version='3.11.3', commit='9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03')), indent=2))
