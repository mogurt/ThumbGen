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

def overview_chart(data,issue_data,output):
    still={r['tool']:r for r in exr_rows(data,issue_data,'w1t4')}
    sequence={r['tool']:r for r in select(data,'exr-sequence8','png','dual','w4t1',['thumbgen'])}
    for tool in ['vips','ffmpeg','magick']:
        sequence[tool]=next(r for r in issue_data if
            (r['cohort'],r['tool'],r['config'],r['profile'])==('exr-sequence8',tool,'w4t1','dual'))
    columns=[
        ('EXR stills','7 images / two sizes','1 job × 4 threads',still),
        ('EXR sequence','8 frames / two sizes','4 jobs × 1 thread',sequence),
        ('Standard images','7 images / two sizes','4 jobs × 1 thread',
         {r['tool']:r for r in select(data,'ordinary-public7','png','dual','w4t1')}),
        ('Tiny-image batch','1,000 copies / one size','4 jobs × 1 thread',
         {r['tool']:r for r in select(data,'tiny-1000','png','425','w4t1')})]
    blue='#2563eb';ink='#172b45';muted='#62748b';gray='#9aa9bb';amber='#b56b13'
    fig=plt.figure(figsize=(14,7.2),facecolor='white')
    ax=fig.add_axes([0,0,1,1]);ax.set_xlim(0,1);ax.set_ylim(0,1);ax.axis('off')
    ax.text(.035,.945,'Fast previews for images and EXR',fontsize=23,fontweight='bold',color=ink)
    ax.text(.035,.898,'Batch time in seconds · lower is faster',fontsize=11,color=muted)
    ax.text(.965,.898,'Windows / i7-13700K / 4 CPU cores',fontsize=10,ha='right',color=muted)
    starts=[.205,.405,.605,.805];width=.16
    row_y=[.70,.59,.48,.37,.26]
    ax.add_patch(Rectangle((.025,.654),.95,.092,facecolor='#edf4ff',edgecolor='none'))
    for i,tool in enumerate(TOOLS):
        ax.text(.035,row_y[i],LABELS[tool],va='center',fontsize=12,
                fontweight='bold' if tool=='thumbgen' else 'normal',color=blue if tool=='thumbgen' else ink)
        if i:ax.plot([.035,.965],[row_y[i]+.055]*2,color='#e6ebf1',lw=.7)
    for col,(title,description,config,rows) in enumerate(columns):
        x=starts[col]
        ax.text(x,.832,title,fontsize=13,fontweight='bold',color=ink)
        ax.text(x,.798,description,fontsize=10,color=muted)
        ax.text(x,.770,config,fontsize=9,color=muted)
        maximum=max(r['median_seconds'] for r in rows.values())
        for i,tool in enumerate(TOOLS):
            y=row_y[i];row=rows.get(tool)
            if row is None:
                ax.text(x,y,'Not supported',fontsize=10,color=muted,va='center')
                continue
            value=row['median_seconds'];issue=row.get('output_issue') in ('wrong-colour','wrong-geometry')
            color=blue if tool=='thumbgen' else amber if issue else gray
            ax.text(x,y+.008,f'{value:.3f} s',fontsize=12,fontweight='bold' if tool=='thumbgen' else 'normal',color=blue if tool=='thumbgen' else ink)
            ax.add_patch(Rectangle((x,y-.013),width,.007,facecolor='#e8edf3',edgecolor='none'))
            ax.add_patch(Rectangle((x,y-.013),width*value/maximum,.007,facecolor=color,edgecolor='none'))
            note=''
            if col==0:
                note={'thumbgen':'7/7 images','ffmpeg':'7/7 images',
                      'vips':'6/7 · color issue','magick':'6/7 · one unreadable'}[tool]
            elif col==1:
                note='8/8 · full display canvas' if tool=='thumbgen' else '8/8 · canvas mismatch'
            if note:ax.text(x,y-.039,note,fontsize=9,color=amber if issue else muted)
    ax.plot([.035,.965],[.195]*2,color='#ccd6e2',lw=.8)
    ax.text(.035,.150,'All outputs: PNG. Five-round medians; bars are scaled within each column.',fontsize=9.5,color=muted)
    ax.text(.035,.114,'Amber bars: output issues. EXR stills: ImageMagick / libvips cannot read the multiresolution image.',fontsize=9.5,color=muted)
    ax.text(.035,.078,'Color, filtering and startup differ between tools. libvips color issues apply to this Magick-loader build.',fontsize=9.5,color=muted)
    ax.text(.035,.035,'2026-11-16–17 · ThumbGen · libvips 8.18.7 · FFmpeg 9.0.1 · ImageMagick 7.1.2-31 · Pillow 12.3',fontsize=8.5,color=muted)
    for ext in ['svg','png']:
        fig.savefig(output/f'five-tool-overview.{ext}',dpi=150,metadata={'Date':None} if ext=='svg' else {})
    plt.close(fig)

def coverage_chart(coverage,output):
    cohorts=['ordinary-public7','png16-synthetic12','exr-public7','exr-sequence8']
    names=['Standard images · 7','Synthetic PNG16 · 12','Still EXRs · 7','Beachball EXR frames · 8']
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
    issue_path=a.results.with_name('issue-timings.json')
    issue_data=json.loads(issue_path.read_text(encoding='utf-8')) if issue_path.exists() else []
    overview_chart(data,issue_data,a.output)
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
