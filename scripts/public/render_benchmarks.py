"""Render README charts from the published comparison data."""
import argparse,json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Patch

TOOLS=['thumbgen','vips','ffmpeg','magick','pillow']
LABELS={'thumbgen':'ThumbGen','vips':'libvips linear','ffmpeg':'FFmpeg','magick':'ImageMagick','pillow':'Pillow'}
COLORS={'thumbgen':'#2563eb','vips':'#0d9488','ffmpeg':'#7c3aed','magick':'#d97706','pillow':'#be4567'}

def select(data,cohort,fmt,profile,config,tools=TOOLS):
    result=[]
    for tool in tools:
        matches=[r for r in data if (r['cohort'],r['format'],r['profile'],r['config'],r['lifecycle'],r['tool'])==(cohort,fmt,profile,config,'batch',tool)]
        if len(matches)!=1:raise ValueError((cohort,fmt,profile,config,tool))
        result.append(matches[0])
    return result

def panel(ax,rows,title,limit=None):
    y=list(range(len(rows)));values=[r['median_seconds'] for r in rows]
    ax.barh(y,values,height=.62,color=[COLORS[r['tool']] for r in rows])
    maximum=limit or max(r['max_seconds'] for r in rows)
    for i,r in enumerate(rows):ax.text(r['median_seconds']+maximum*.025,i,f"{r['median_seconds']:.3f}",va='center',fontsize=11)
    ax.set_yticks(y,[LABELS[r['tool']] for r in rows],fontsize=11);ax.invert_yaxis()
    ax.set_xlim(0,maximum*1.25);ax.set_xlabel('Seconds per batch',fontsize=10)
    ax.set_title(title,loc='left',fontsize=12,fontweight='bold',pad=14)
    ax.grid(axis='x',alpha=.18);ax.set_axisbelow(True)
    for side in ['top','right','left']:ax.spines[side].set_visible(False)
    ax.tick_params(axis='y',length=0,pad=8)

def exr_rows(data,issue_data,config):
    rows=select(data,'exr-public7','png','dual',config,['thumbgen','ffmpeg'])
    im=select(data,'exr-public7-supporters3','png','dual',config,['magick'])[0]
    rows.append({**im,'display_note':'6/7 images · multiresolution EXR unavailable'})
    matches=[r for r in issue_data if r['cohort']=='exr-public7-supporters3' and r['tool']=='vips' and r['config']==config and r['profile']=='dual']
    if len(matches)>1:raise ValueError('Duplicate libvips EXR timing')
    if matches:rows.append({**matches[0],'display_note':'6/7 images · HDR color issue'})
    return rows

def exr_panel(ax,rows,title,limit):
    for row in rows:
        y=TOOLS.index(row['tool']);value=row['median_seconds']
        ax.barh(y,value,height=.5,color=COLORS[row['tool']])
        ax.text(value+limit*.025,y,f'{value:.3f}',va='center',fontsize=11)
        if row.get('display_note'):
            ax.text(limit*.025,y+.36,row['display_note'],va='center',fontsize=8.5,color='#64748b')
    shown={r['tool'] for r in rows}
    for tool,label in [('vips','HDR color issue'),('pillow','EXR unavailable')]:
        if tool in shown:continue
        ax.text(limit*.025,TOOLS.index(tool),label,va='center',fontsize=11,color='#64748b')
    ax.set_yticks(range(5),[LABELS[t] for t in TOOLS],fontsize=11);ax.set_ylim(4.6,-.6)
    ax.set_xlim(0,limit*1.25);ax.set_xlabel('Seconds per batch',fontsize=10)
    ax.set_title(title,loc='left',fontsize=12,fontweight='bold',pad=14)
    ax.grid(axis='x',alpha=.18);ax.set_axisbelow(True)
    for side in ['top','right','left']:ax.spines[side].set_visible(False)
    ax.tick_params(axis='y',length=0,pad=8)

def coverage_chart(coverage,output):
    cohorts=['ordinary-public7','png16-synthetic12','exr-public7','exr-sequence8']
    names=['Everyday images · 7','Synthetic PNG16 · 12','Still EXRs · 7','Beachball EXR frames · 8']
    palette={'valid':'#d1fae5','unsupported':'#e2e8f0','wrong-colour':'#fde68a','wrong-geometry':'#fed7aa','read-failure':'#fecdd3'}
    fig,ax=plt.subplots(figsize=(13,5.5))
    for y,cohort in enumerate(cohorts):
        for x,tool in enumerate(TOOLS):
            row=next(r for r in coverage if (r['cohort'],r['tool'],r['format'],r['profile'],r['config'],r['lifecycle'])==(cohort,tool,'png','dual','w1t4','batch'))
            status=row['support']; n=row['input_count']
            text='Supported' if status=='valid' else {'unsupported':'Unsupported','wrong-colour':'Corrupted highlights','wrong-geometry':'Wrong canvas'}[status]
            if cohort=='exr-public7' and tool=='vips':text='6 corrupted highlights\n1 read failure'
            if cohort=='exr-public7' and tool=='magick':status='valid';text='6/7 supported'
            if cohort=='png16-synthetic12' and tool=='pillow':text+='\n8-bit path'
            ax.add_patch(Rectangle((x,y),1,1,facecolor=palette[status],edgecolor='white',linewidth=4))
            if cohort=='exr-public7' and tool=='vips':ax.add_patch(Rectangle((x,y+.91),1,.09,facecolor=palette['read-failure'],edgecolor='none'))
            if cohort=='exr-public7' and tool=='magick':
                ax.add_patch(Rectangle((x,y+6/7),1,1/7,facecolor=palette['read-failure'],edgecolor='none'))
                ax.text(x+.5,y+.42,text,ha='center',va='center',fontsize=10,color='#0f172a')
                ax.text(x+.5,y+.7,'Multiresolution EXR unavailable',ha='center',va='center',fontsize=8.5,color='#475569')
            else:ax.text(x+.5,y+.5,text,ha='center',va='center',fontsize=10,color='#0f172a')
    ax.set_xlim(0,5);ax.set_ylim(4,0)
    ax.set_xticks([x+.5 for x in range(5)],[LABELS[t] for t in TOOLS],fontsize=11)
    ax.xaxis.tick_top();ax.set_yticks([y+.5 for y in range(4)],names,fontsize=10)
    ax.tick_params(length=0,pad=12)
    for spine in ax.spines.values():spine.set_visible(False)
    fig.suptitle('Image support at a glance',x=.035,ha='left',fontsize=22,fontweight='bold')
    fig.text(.035,.89,'Public image examples · color and precision vary between tools',fontsize=10,color='#475569')
    labels=['Supported','Decoder unavailable','Color issue','Canvas mismatch','Read error']
    fig.legend([Patch(facecolor=palette[k]) for k in palette],labels,loc='lower left',bbox_to_anchor=(.03,.105),ncol=5,frameon=False,fontsize=9)
    fig.text(.035,.075,'Canvas mismatch: the full EXR display window is not restored.',fontsize=9,color='#475569')
    fig.text(.035,.04,'The EXR color issue applies to this libvips Magick loader. Source images and settings: comparison guide.',fontsize=9,color='#475569')
    fig.subplots_adjust(left=.205,right=.985,top=.76,bottom=.23)
    for ext in ['svg','png']:fig.savefig(output/f'five-tool-coverage.{ext}',dpi=150,metadata={'Date':None} if ext=='svg' else {})
    plt.close(fig)

def main():
    p=argparse.ArgumentParser();p.add_argument('--results',type=Path,default=Path(__file__).resolve().parents[2]/'docs/performance/aggregates.json');p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[2]/'docs/assets');a=p.parse_args()
    data=json.loads(a.results.read_text(encoding='utf-8'));a.output.mkdir(parents=True,exist_ok=True)
    plt.rcParams.update({'svg.fonttype':'none','font.family':'DejaVu Sans','axes.labelcolor':'#475569','text.color':'#0f172a','svg.hashsalt':'thumbgen-five-tool-20261116'})
    fig,axes=plt.subplots(2,2,figsize=(13,9.6))
    panels=[('ordinary-public7','png','425','7 inputs / one PNG size'),('ordinary-public7','png','dual','7 inputs / two PNG sizes'),('ordinary-public7','jpeg','dual','7 inputs / two JPEG sizes'),('tiny-1000','png','425','1,000 tiny PNG copies / one size')]
    for ax,(cohort,fmt,profile,title) in zip(axes.flat,panels):panel(ax,select(data,cohort,fmt,profile,'w4t1'),title)
    fig.suptitle('Five ways to make previews',x=.045,ha='left',fontsize=22,fontweight='bold')
    fig.text(.045,.925,'4 CPU cores · 4 parallel jobs · batch time in seconds',fontsize=10,color='#475569')
    fig.text(.045,.065,'Shorter is faster. Color, transparency and filtering differ between tools.',fontsize=9,color='#475569')
    fig.text(.045,.043,'JPEG settings differ: FFmpeg qscale=2; other tools Q90. See the comparison guide for details.',fontsize=9,color='#475569')
    fig.text(.045,.021,'2026-11-16 · ThumbGen · libvips 8.18.7 · FFmpeg 9.0.1 · ImageMagick 7.1.2-31 · Pillow 12.3',fontsize=8.5,color='#475569')
    fig.subplots_adjust(left=.14,right=.96,top=.85,bottom=.15,hspace=.65,wspace=.55)
    for ext in ['svg','png']:fig.savefig(a.output/f'five-tool-overview.{ext}',dpi=150,metadata={'Date':None} if ext=='svg' else {})
    plt.close(fig)
    issue_path=a.results.with_name('issue-timings.json')
    issue_data=json.loads(issue_path.read_text(encoding='utf-8')) if issue_path.exists() else []
    fig,axes=plt.subplots(1,2,figsize=(13,6.2));sets=[exr_rows(data,issue_data,config) for config in ['w1t4','w4t1']]
    limit=max(r['max_seconds'] for rows in sets for r in rows)
    for ax,rows,title in zip(axes,sets,['1 job × 4 threads','4 jobs × 1 thread']):exr_panel(ax,rows,title,limit)
    fig.suptitle('EXR previews / two output sizes',x=.045,ha='left',fontsize=21,fontweight='bold')
    fig.text(.045,.88,'7 public images · 4 CPU cores',fontsize=10,color='#475569')
    fig.text(.045,.11,'ImageMagick and libvips: 6/7 images (multiresolution EXR unavailable). ThumbGen and FFmpeg: 7/7 images.',fontsize=9,color='#475569')
    fig.text(.045,.065,'Image colors and filtering differ. See the comparison guide for sample downloads and settings.',fontsize=9,color='#475569')
    fig.text(.045,.022,'2026-11-16–17 · ThumbGen · libvips 8.18.7 · FFmpeg 9.0.1 · ImageMagick 7.1.2-31 Q16-HDRI, no OpenMP',fontsize=8.5,color='#475569')
    fig.subplots_adjust(left=.14,right=.96,top=.7,bottom=.27,wspace=.55)
    for ext in ['svg','png']:fig.savefig(a.output/f'five-tool-exr.{ext}',dpi=150,metadata={'Date':None} if ext=='svg' else {})
    plt.close(fig)

    coverage_chart(json.loads(a.results.with_name('coverage.json').read_text(encoding='utf-8')),a.output)
    for name in ('five-tool-overview.svg','five-tool-exr.svg','five-tool-coverage.svg'):
        path=a.output/name
        path.write_text('\n'.join(line.rstrip() for line in path.read_text(encoding='utf-8').splitlines())+'\n',encoding='utf-8',newline='\n')

if __name__=='__main__':main()
