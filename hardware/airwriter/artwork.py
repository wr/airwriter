"""Emit the back-face cover artwork, filtered against whatever copper is on the board.

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        artwork.py [source_dir]

Run AFTER route.py, stitch.py and the via prune -- never before. It also lays down the
TOUCH GUARD, because KiCad exports every rule area to Freerouting as a hard keepout
regardless of its tracks/vias settings, so a pour-only guard still blocks the router. The cells are placed
around finished copper, and the rule areas they emit bar copper pour only, so nothing
here forces another route. Running it before routing does not work: Freerouting reads
a multi-outline zone as a single keepout and drives straight through the artwork.
"""
import pcbnew, re, math, sys, uuid, os
SRC = sys.argv[1] if len(sys.argv)>1 else os.path.expanduser('~/Downloads/kicad')
S   = 0.52                      # 100 x 150mm source -> 52 x 78mm board
HERE= os.path.dirname(os.path.abspath(__file__))
BRD = os.path.join(HERE,'airwriter.kicad_pcb')

def cells(name):
    s=open(os.path.join(SRC,name+'.svg')).read(); out=[]
    for m in re.finditer(r'<rect[^>]*>', s):
        t=m.group(0); g=lambda k: re.search(r'\b%s="([-\d.]+)"'%k,t)
        if not all(g(k) for k in ('x','y','width','height')): continue
        x,y,w,h=(float(g(k).group(1)) for k in ('x','y','width','height'))
        out.append((152-(x+w)*S, 60+y*S, 152-x*S, 60+(y+h)*S))   # mirrored for the back
    return out

b=pcbnew.LoadBoard(BRD)
F,BK=pcbnew.F_Cu,pcbnew.B_Cu
cop={F:[],BK:[]}; padsB=[]; nongndB=[]; silk=[]
for t in b.GetTracks():
    if isinstance(t,pcbnew.PCB_VIA):
        c=t.GetPosition(); q=(c.x/1e6,c.y/1e6,t.GetWidth()/2/1e6)
        for L in cop: cop[L].append(q)
        padsB.append(q)
        if t.GetNetname()!='/GND': nongndB.append(q)
    else:
        L=t.GetLayer()
        if L not in cop: continue
        s0,e0=t.GetStart(),t.GetEnd(); w=t.GetWidth()/2/1e6
        n=max(1,int(math.hypot(e0.x-s0.x,e0.y-s0.y)/1e6/0.25))
        for i in range(n+1):
            q=(s0.x/1e6+(e0.x-s0.x)/1e6*i/n, s0.y/1e6+(e0.y-s0.y)/1e6*i/n, w)
            cop[L].append(q)
            if L==BK and t.GetNetname()!='/GND': nongndB.append(q)
for f in b.GetFootprints():
    for p in f.Pads():
        c=p.GetCenter(); q=(c.x/1e6,c.y/1e6,max(p.GetSize().x,p.GetSize().y)/2/1e6)
        thru=p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH,pcbnew.PAD_ATTRIB_NPTH)
        for L in cop:
            if thru or p.IsOnLayer(L): cop[L].append(q)
        if thru or p.IsOnLayer(BK):
            padsB.append(q)
            if p.GetNetname()!='/GND': nongndB.append(q)
SILK={pcbnew.F_SilkS,pcbnew.B_SilkS}
def note(it):
    try:
        if it.GetLayer() in SILK and it.IsVisible():
            bb=it.GetBoundingBox()
            silk.append((bb.GetLeft()/1e6,bb.GetTop()/1e6,bb.GetRight()/1e6,bb.GetBottom()/1e6))
    except Exception: pass
for d in b.GetDrawings(): note(d)
for f in b.GetFootprints():
    for g in list(f.GraphicalItems())+[f.Reference(),f.Value()]: note(g)
led=[]
for r in ('D1','D2','D3'):
    f=[g for g in b.GetFootprints() if g.GetReference()==r][0]
    p=f.GetPosition(); w,h=(3.4,3.0) if r in ('D1','D2') else (1.6,2.2)
    led.append((p.x/1e6-w/2,p.y/1e6-h/2,p.x/1e6+w/2,p.y/1e6+h/2))

def clear(k,obs,m):
    x0,y0,x1,y1=k
    for ox,oy,r in obs:
        if x0-r-m<ox<x1+r+m and y0-r-m<oy<y1+r+m: return False
    return True
def nosilk(k):
    x0,y0,x1,y1=k
    for a,c,e,g in silk:
        if x0-0.12<e and x1+0.12>a and y0-0.12<g and y1+0.12>c: return False
    return True

fr4_logo =[k for k in cells('logo_fr4_keepout')  if clear(k,cop[F],0.2) and clear(k,cop[BK],0.2) and nosilk(k)]
fr4_grain=[k for k in cells('grain_fr4_keepout') if clear(k,cop[BK],0.2) and nosilk(k)]
gold=[]
for n in ('logo_mask_open','grain_mask_open'):
    gold+= [k for k in cells(n) if clear(k,nongndB,0.2) and clear(k,padsB,0.15) and nosilk(k)]
silkcells=cells('logo_silk')+cells('grain_silk')

def zone(name,layers,polys):
    P='\n'.join('\t\t(polygon (pts (xy %.4f %.4f) (xy %.4f %.4f) (xy %.4f %.4f) (xy %.4f %.4f)))'
                %(k[0],k[1],k[2],k[1],k[2],k[3],k[0],k[3]) for k in polys)
    return ('\t(zone\n\t\t(layers %s)\n\t\t(uuid "%s")\n\t\t(name "%s")\n\t\t(hatch full 0.508)\n'
            '\t\t(connect_pads (clearance 0))\n\t\t(min_thickness 0.254)\n'
            '\t\t(keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n'
            '\t\t(placement (enabled no) (sheetname ""))\n'
            '\t\t(fill (thermal_gap 0.508) (thermal_bridge_width 0.508))\n%s\n\t)'%(layers,uuid.uuid4(),name,P))
def rect(k,l):
    return ('\t(gr_rect (start %.4f %.4f) (end %.4f %.4f) (stroke (width 0.02) (type solid)) '
            '(fill solid) (layer "%s") (uuid "%s"))'%(k[0],k[1],k[2],k[3],l,uuid.uuid4()))

s=open(BRD).read()

# the touch guard is a pour keepout, but KiCad exports every rule area to the router
# as a hard keepout -- so it, like the artwork, goes on only after routing
import math as _m
GX,GY,GR = 136.30, 121.80, 6.1
_oct=' '.join('(xy %.1f %.1f)'%(GX+GR*_m.cos(_m.radians(a)), GY+GR*_m.sin(_m.radians(a))) for a in range(0,360,45))
guard=('\t(zone\n\t\t(layers "F.Cu" "B.Cu")\n\t\t(uuid "%s")\n\t\t(name "TOUCH GUARD")\n\t\t(hatch full 0.508)\n'
       '\t\t(connect_pads (clearance 0))\n\t\t(min_thickness 0.254)\n'
       '\t\t(keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n'
       '\t\t(placement (enabled no) (sheetname ""))\n'
       '\t\t(fill (thermal_gap 0.508) (thermal_bridge_width 0.508))\n'
       '\t\t(polygon (pts %s))\n\t)'%(uuid.uuid4(), _oct))
gr=[]
for k in fr4_logo: gr += [rect(k,'B.Mask'), rect(k,'F.Mask')]     # lit: open both faces
for k in fr4_grain: gr.append(rect(k,'B.Mask'))
for k in gold:      gr.append(rect(k,'B.Mask'))
for k in silkcells: gr.append(rect(k,'B.SilkS'))
for k in led:
    m=(k[0]+0.2,k[1]+0.2,k[2]-0.2,k[3]-0.2)
    gr += [rect(m,'B.Mask'), rect(m,'F.Mask')]
i=s.rindex('\n\t(zone')
j=s.index('\n\t)',s.index('(polygon',i))+3
s=s[:j]+'\n'+guard+'\n'+zone('ARTWORK FR4 WORDMARK','"F.Cu" "B.Cu"',fr4_logo)+'\n'+zone('ARTWORK FR4 GRAIN','"B.Cu"',fr4_grain)+s[j:]
a=s.rindex('\n\t(gr_text ') if '\n\t(gr_text ' in s else s.rindex('\n\t(zone')
s=s[:a+1]+'\n'.join(gr)+'\n'+s[a+1:]
open(BRD,'w').write(s)
print('wordmark FR4 %d, grain FR4 %d, gold %d, silk %d, LED windows %d'
      %(len(fr4_logo),len(fr4_grain),len(gold),len(silkcells),len(led)))
