#!/usr/bin/env python3
"""Round-10 finish: clearance-aware test-point placement + antenna stitch helper.
Reusable geometry model. Import or run with --report."""
import pcbnew
from pcbnew import VECTOR2I
import sys

MM = 1_000_000
def mm(v): return int(round(v*MM))
def um(v): return round(v/MM,3)

class Model:
    def __init__(self, path):
        self.b = pcbnew.LoadBoard(path)
        self.FCu = self.b.GetLayerID('F.Cu')
        self.BCu = self.b.GetLayerID('B.Cu')
        self._build()
    def _build(self):
        b=self.b
        # copper obstacles on F.Cu: (netcode, shape)
        self.fcu=[]      # list of (netcode, shape, bbox)
        self.bcu=[]
        def bbx(item):
            bb=item.GetBoundingBox(); return (bb.GetLeft(),bb.GetTop(),bb.GetRight(),bb.GetBottom())
        for t in b.GetTracks():   # tracks + vias
            nc=t.GetNetCode()
            if t.IsOnLayer(self.FCu):
                self.fcu.append((nc,t.GetEffectiveShape(self.FCu),bbx(t)))
            if t.IsOnLayer(self.BCu):
                self.bcu.append((nc,t.GetEffectiveShape(self.BCu),bbx(t)))
        for f in b.GetFootprints():
            for pad in f.Pads():
                nc=pad.GetNetCode()
                if pad.IsOnLayer(self.FCu):
                    self.fcu.append((nc,pad.GetEffectiveShape(self.FCu),bbx(pad)))
                if pad.IsOnLayer(self.BCu):
                    self.bcu.append((nc,pad.GetEffectiveShape(self.BCu),bbx(pad)))
        # courtyards
        self.cyards=[]   # (ref, polyset)
        for f in b.GetFootprints():
            cy=f.GetCourtyard(self.FCu)
            if cy.OutlineCount()>0:
                self.cyards.append((f.GetReference(),cy))
        # antenna keepout outline(s)
        self.keepouts=[]
        for f in b.GetFootprints():
            for z in f.Zones():
                if z.GetIsRuleArea():
                    self.keepouts.append(z.Outline())
        for z in b.Zones():
            if z.GetIsRuleArea():
                self.keepouts.append(z.Outline())
        # GND F.Cu zone filled polys
        self.gnd_fcu=[]
        self.gnd_bcu=[]
        for z in b.Zones():
            if z.GetNetname()=='/GND':
                if z.IsOnLayer(self.FCu):
                    self.gnd_fcu.append(z.GetFilledPolysList(self.FCu))
                if z.IsOnLayer(self.BCu):
                    self.gnd_bcu.append(z.GetFilledPolysList(self.BCu))

    def copper_clear(self, P, netcode, clr_nm, layer='F'):
        """True if no OTHER-net copper within clr_nm of point P on given layer."""
        pool = self.fcu if layer=='F' else self.bcu
        px,py=P.x,P.y
        for nc,sh,bb in pool:
            if nc==netcode: continue
            if px<bb[0]-clr_nm or px>bb[2]+clr_nm or py<bb[1]-clr_nm or py>bb[3]+clr_nm: continue
            if sh.Collide(P, clr_nm):
                return False
        return True
    def nearest_other(self, P, netcode, layer='F'):
        """min distance (nm) to nearest other-net copper by probing."""
        lo,hi=0,mm(3.0)
        # linear-ish: find smallest clr that is clear
        pool=self.fcu if layer=='F' else self.bcu
        best=hi
        px,py=P.x,P.y
        for nc,sh,bb in pool:
            if nc==netcode: continue
            if px<bb[0]-hi or px>bb[2]+hi or py<bb[1]-hi or py>bb[3]+hi: continue
            # binary search distance
            a,c=0,mm(3.0)
            if not sh.Collide(P,c):
                continue
            for _ in range(14):
                mmid=(a+c)//2
                if sh.Collide(P,mmid): c=mmid
                else: a=mmid
            best=min(best,c)
        return best
    def courtyard_clear(self, P, r_nm, skip=None):
        for ref,cy in self.cyards:
            if skip and ref in skip: continue
            if cy.Collide(P, r_nm):
                return False, ref
        return True, None
    def keepout_clear(self, P, r_nm):
        for ol in self.keepouts:
            if ol.Collide(P, r_nm):
                return False
        return True
    def on_net(self, P, netcode, layer='F'):
        pool=self.fcu if layer=='F' else self.bcu
        for nc,sh,bb in pool:
            if nc==netcode and sh.Collide(P,0):
                return True
        return False
    def in_gnd(self, P, layer='F'):
        polys=self.gnd_fcu if layer=='F' else self.gnd_bcu
        for pl in polys:
            if pl.Contains(P):
                return True
        return False

    def net_track_points(self, netcode, layer='F'):
        """sample points along tracks of net on layer."""
        b=self.b; L=self.FCu if layer=='F' else self.BCu
        pts=[]
        for t in b.GetTracks():
            if t.GetNetCode()!=netcode: continue
            if t.Type()==pcbnew.PCB_VIA_T: continue
            if not t.IsOnLayer(L): continue
            s=t.GetStart(); e=t.GetEnd()
            import math
            dx=e.x-s.x; dy=e.y-s.y
            ln=math.hypot(dx,dy)
            n=max(1,int(ln/ (0.3*MM)))
            for i in range(n+1):
                pts.append(VECTOR2I(int(s.x+dx*i/n), int(s.y+dy*i/n)))
        # pad centers
        for f in b.GetFootprints():
            for pad in f.Pads():
                if pad.GetNetCode()==netcode and pad.IsOnLayer(L):
                    pp=pad.GetPosition(); pts.append(VECTOR2I(pp.x,pp.y))
        return pts

RPAD=mm(0.5)
CLR=mm(0.2)
CYR=mm(1.0)   # my courtyard radius

def validate(M, P, netcode, need_gnd=False, need_onnet=True, extra=0.0, skip_cy=None):
    clr=RPAD+CLR+mm(extra)
    if not M.copper_clear(P, netcode, clr): return False,'copper'
    ok,ref=M.courtyard_clear(P, CYR, skip=skip_cy)
    if not ok: return False,f'crtyd:{ref}'
    if not M.keepout_clear(P, RPAD): return False,'keepout'
    if need_gnd:
        if not M.in_gnd(P): return False,'not-in-gnd'
    elif need_onnet:
        if not M.on_net(P, netcode): return False,'off-net'
    return True,'ok'

if __name__=='__main__':
    M=Model('airwriter.kicad_pcb')
    print("obstacles F.Cu:",len(M.fcu),"courtyards:",len(M.cyards),"keepouts:",len(M.keepouts))
    b=M.b
    specs=[
        ('VBUS','/VBUS',False),
        ('3V3','/+3V3',False),
        ('EN','/EN',False),
        ('LED_DATA5','/LED_DATA5',False),
    ]
    for label,net,gnd in specs:
        nc=b.FindNet(net).GetNetCode()
        pts=M.net_track_points(nc,'F')
        best=None
        for P in pts:
            ok,why=validate(M,P,nc,need_gnd=gnd)
            if ok:
                d=M.nearest_other(P,nc)
                if best is None or d>best[1]:
                    best=(P,d)
        if best:
            P,d=best
            print(f"{label:10} best ({um(P.x)},{um(P.y)}) margin={um(d)}mm  (candidates {len(pts)})")
        else:
            print(f"{label:10} NO valid on-net point among {len(pts)} samples")
