"""Generate owned Deep colour fixtures with an independent Decimal interval oracle."""
import argparse
from decimal import Decimal, localcontext
import json
from pathlib import Path
import random
import struct


def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
def sample(z,back,colour,alpha):
    if isinstance(alpha,(int,float)):alpha=[alpha]*3
    return dict(z=f32(z),zback=f32(back),colour=list(map(f32,colour)),alpha=list(map(f32,alpha)))


def reference(samples):
    D=Decimal.from_float;zero=Decimal(0);one=Decimal(1)
    with localcontext() as context:
        context.prec=100
        samples=[dict(z=D(s['z']),zback=max(D(s['z']),D(s['zback'])),colour=list(map(D,s['colour'])),alpha=list(map(D,s['alpha']))) for s in samples]
        depths=sorted({s[k] for s in samples for k in ('z','zback')});colour=[zero]*3;through=[one]*3
        def merge(items):
            output_c=[];output_a=[]
            for channel in range(3):
                opaque=[c[channel] for c,a in items if a[channel]==one]
                if opaque:output_c.append(sum(opaque)/len(opaque));output_a.append(one);continue
                tau=zero;emission=zero;transmission=one
                for c,a in items:
                    extinction=-(one-a[channel]).ln() if a[channel] else zero
                    tau+=extinction;emission+=c[channel]*(extinction/a[channel] if a[channel] else one);transmission*=one-a[channel]
                alpha=one-transmission
                output_c.append(emission*alpha/tau if tau else emission);output_a.append(alpha)
            return output_c,output_a
        def over(items):
            if not items:return
            c,a=merge(items)
            for channel in range(3):colour[channel]+=through[channel]*c[channel];through[channel]*=one-a[channel]
        for index,depth in enumerate(depths):
            over([(s['colour'],s['alpha']) for s in samples if s['z']==s['zback']==depth])
            if index+1==len(depths):continue
            following=depths[index+1];pieces=[]
            for s in samples:
                if s['z']<=depth<following<=s['zback'] and s['z']<s['zback']:
                    fraction=(following-depth)/(s['zback']-s['z'])
                    alpha=[one-((one-a).ln()*fraction).exp() if a<one else one for a in s['alpha']]
                    rgb=[c*(part/a if a else fraction) for c,part,a in zip(s['colour'],alpha,s['alpha'])]
                    pieces.append((rgb,alpha))
            over(pieces)
        return dict(colour=list(map(float,colour)),alpha=[float(one-t) for t in through])


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    cases=[]
    def add(name,samples):cases.append(dict(id=name,samples=samples,expected=reference(samples)))
    add('empty',[])
    add('reversed-points',[sample(2,2,[0,0,.5],.5),sample(1,1,[.5,0,0],.5)])
    add('coincident-points',[sample(1,1,[.5,0,0],.5),sample(1,1,[0,0,.5],.5)])
    add('coincident-volumes',[sample(0,1,[.5,0,0],.5),sample(0,1,[0,0,.5],.5)])
    add('partial-overlap',[sample(0,2,[.75,0,0],.75),sample(1,3,[0,0,.75],.75)])
    add('point-inside-volume',[sample(0,2,[.75,0,0],.75),sample(1,1,[0,.5,0],.5)])
    add('point-before-volume-at-front',[sample(1,3,[0,0,.5],.5),sample(1,1,[.5,0,0],.5)])
    add('unequal-density',[sample(0,1,[.75,0,0],.75),sample(0,1,[0,0,.5],.5)])
    add('zero-alpha-emission',[sample(0,2,[2,0,0],0),sample(1,1,[0,.5,0],.5)])
    add('absorbed-emission',[sample(0,1,[1,0,0],0),sample(0,1,[0,0,.75],.75)])
    add('three-opaque-tie',[sample(0,1,[1,0,0],1),sample(0,1,[0,1,0],1),sample(0,1,[0,0,1],1)])
    add('coloured-opacity',[sample(1,1,[.5,.25,.125],[.5,.25,.125]),sample(2,2,[.25,.5,.75],[.25,.5,.75])])
    add('tiny-volume-alpha',[sample(0,2,[1,0,0],1e-30),sample(1,1,[0,0,0],0)])
    add('tiny-colour',[sample(1,1,[1e-30,0,0],.5),sample(2,2,[0,0,0],.5)])
    add('negative-hdr',[sample(1,1,[-2,5,.125],.5),sample(2,2,[4,-1,2],.5)])
    add('reversed-interval-is-point',[sample(3,2,[0,0,.5],.5),sample(1,1,[.5,0,0],.5)])
    add('near-opaque-transmission',[sample(z,z,[0,0,0],1-2**-24) for z in (0,1,2)]+[sample(3,3,[0,1e30,0],0)])
    rng=random.Random(94198)
    for index in range(100):
        samples=[]
        for _ in range(rng.randrange(1,9)):
            z=rng.randrange(8);back=z+rng.randrange(5)
            samples.append(sample(z,back,[rng.randrange(-4,17)/8 for _ in range(3)],[rng.choice([0,.125,.25,.5,.75,1]) for _ in range(3)]))
        rng.shuffle(samples);add('seeded-%03d'%index,samples)
    output=dict(schema_version=1,policy='deep-colour-sweep-v1',precision=100,seed=94198,
                reference='Decimal direct subdivision and per-channel optical mixture; no OIIO or product code',
                opaque_tie_policy='equal-weight n-way mean of fully opaque contributors',cases=cases)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8',newline='\n') as stream:json.dump(output,stream,indent=2);stream.write('\n')
    print(json.dumps(dict(cases=len(cases),decimal_precision=100)))


if __name__=='__main__':main()
