"""Compose backgrounds in the Decimal100 oracle before any alpha/colour rounding."""
import argparse
import hashlib
import json
from pathlib import Path

from generate_deep_reference import reference


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    original=Path(__file__).parent/'data/deep-reference-v1.json'
    data=json.loads(original.read_text(encoding='utf-8'))
    backgrounds=[[0.0,0.0,0.0],[1.0,1.0,1.0],[1e30,-0.5,0.25],[10000.0,0.001,2.0]]
    cases=[dict(id=case['id'],expected=[reference(case['samples'],background)['colour'] for background in backgrounds])
           for case in data['cases']]
    result=dict(schema_version=1,precision=100,source_reference_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
                backgrounds=backgrounds,reference='Decimal source-basis colour + unrounded transmission * background',cases=cases)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8',newline='\n') as stream:
        json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(dict(cases=len(cases),composites=len(cases)*len(backgrounds))))


if __name__=='__main__':main()
