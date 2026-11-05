"""Independent references for positive-infinity point depths, never JSON Infinity.

All volumes are finite. Moving the infinity points to a double depth just past
every finite endpoint cannot intersect any volume or alter any finite length.
The existing Decimal100 subdivision oracle then evaluates a purely finite scene.
Transmission is independently the Decimal product of all sample transparencies.
"""
import argparse
from decimal import Decimal, localcontext
import json
import math
from pathlib import Path
import struct
from generate_deep_reference import reference, sample

INF=float('inf')


def shifted(samples):
    if any(math.isnan(s[k]) or s[k]<0 for s in samples for k in ('z','zback')):
        raise ValueError('reference only accepts nonnegative, non-NaN depths')
    if any(math.isfinite(s['z']) and math.isinf(s['zback']) for s in samples):
        raise ValueError('an infinite volume cannot be replaced by a finite interval')
    finite=[s[k] for s in samples for k in ('z','zback') if math.isfinite(s[k])]
    right=math.nextafter(max([0.]+finite),INF)
    assert math.isfinite(right)
    return [dict(s,z=right if math.isinf(s['z']) else s['z'],zback=right if math.isinf(s['zback']) else s['zback']) for s in samples]


def expectation(samples,background=None):return reference(shifted(samples),background)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    cases=[];backgrounds=[[0.,0.,0.],[1.,1.,1.],[1e30,-.5,.25]]
    def add(name,samples):
        with localcontext() as context:
            context.prec=100;through=[Decimal(1)]*3
            for s in samples:
                for c in range(3):through[c]*=1-Decimal.from_float(s['alpha'][c])
        encoded=[dict(s,z='positive_infinity' if math.isinf(s['z']) else s['z'],zback='positive_infinity' if math.isinf(s['zback']) else s['zback']) for s in samples]
        cases.append(dict(id=name,samples=encoded,expected=expectation(samples),transmission=list(map(float,through)),
                          backgrounds=[expectation(samples,b)['colour'] for b in backgrounds]))
    add('single-infinite-point',[sample(INF,INF,[.5,0,.25],.5)])
    add('reversed-infinite-is-point',[sample(INF,0,[.5,0,.25],.5)])
    add('finite-point-before-infinite',[sample(INF,INF,[0,0,.5],.5),sample(1,1,[.5,0,0],.5)])
    add('finite-volume-before-infinite',[sample(INF,INF,[0,.5,0],.5),sample(0,2,[.75,0,0],.75)])
    add('overlap-before-infinite',[sample(0,2,[.75,0,0],.75),sample(1,3,[0,0,.75],.75),sample(INF,INF,[0,.5,0],.5)])
    add('coincident-infinite-points',[sample(INF,INF,[.5,0,0],.5),sample(INF,INF,[0,0,.25],.25)])
    add('opaque-infinite-tie',[sample(INF,INF,rgb,1) for rgb in ([1,0,0],[0,1,0],[0,0,1])])
    add('hidden-infinite-emission',[sample(1,1,[0,.5,0],1),sample(INF,INF,[0,0,1e30],0)])
    add('infinite-emission',[sample(INF,INF,[2,0,0],0)])
    maximum=struct.unpack('<f',bytes.fromhex('ffff7f7f'))[0]
    add('maximum-finite-depth',[sample(maximum,maximum,[.5,0,0],.5),sample(INF,INF,[0,0,.5],.5)])
    add('coloured-infinite-opacity',[sample(INF,INF,[.5,.25,.125],[.5,.25,.125]),sample(1,1,[.25,.5,.75],[.25,.5,.75])])
    add('tiny-transmission-infinite-hdr',[sample(z,z,[0,0,0],1-2**-24) for z in (0,1,2)]+[sample(INF,INF,[0,1e30,0],0)])
    add('opaque-and-partial-at-infinity',[sample(INF,INF,[.5,0,0],.5),sample(INF,INF,[0,.5,0],1)])
    add('finite-emission-and-infinite',[sample(0,2,[2,0,0],0),sample(1,1,[0,.5,0],.5),sample(INF,INF,[0,0,.25],.25)])
    add('finite-volume-to-maximum-depth',[sample(0,maximum,[.75,0,0],.75),sample(INF,maximum,[0,0,.5],.5)])
    result=dict(schema_version=1,precision=100,policy='positive-infinity-points-v1',
        reference='Finite Decimal100 subdivision after moving only infinite points past all finite endpoints; independent transparency product',
        backgrounds=backgrounds,cases=cases)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8',newline='\n') as stream:json.dump(result,stream,indent=2,allow_nan=False);stream.write('\n')
    print(json.dumps(dict(cases=len(cases),background_comparisons=len(cases)*len(backgrounds),precision=100)))


if __name__=='__main__':main()
