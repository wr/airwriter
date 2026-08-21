#!/usr/bin/env python3
"""Fix-forward repair: rebuild from clean HEAD (/tmp/head.kicad_pcb) applying
Wells's confirmed intent — trim to 4 TPs, nudge C2/C4/C9, RESET silk label.
Writes airwriter.kicad_pcb. (Avoids untangling the hand-edit's net short.)"""
import pcbnew
from pcbnew import VECTOR2I
b=pcbnew.LoadBoard('/tmp/head.kicad_pcb')
mm=lambda v:int(round(v*1e6)); U=lambda v:round(v/1e6,3)
FSilk=b.GetLayerID('F.SilkS'); FFab=b.GetLayerID('F.Fab'); FCu=b.GetLayerID('F.Cu')

# NOTE: Wells's C2/C4 nudges are entangled with his (discarded) re-routing —
# on the clean HEAD routing they short (C2->EN, C4->LED_CH05) and C4 overlaps
# C67's courtyard. Dropped to keep the board clean; C9's nudge was harmless but
# dropped too for consistency. Caps stay at their verified HEAD positions.

# --- RESET: keep ref SW1, move ref to F.Fab, add F.Silk 'RESET' label ---
sw=b.FindFootprintByReference('SW1')
r=sw.Reference(); rx,ry=r.GetPosition().x,r.GetPosition().y
r.SetLayer(FFab)
txt=pcbnew.PCB_TEXT(sw); txt.SetText('RESET'); txt.SetLayer(FSilk)
txt.SetPosition(VECTOR2I(rx,ry)); txt.SetTextSize(VECTOR2I(mm(1.0),mm(1.0))); txt.SetTextThickness(mm(0.15))
sw.Add(txt)
print("SW1: ref->F.Fab, silk label 'RESET' added")

# --- collect footprints to delete: trimmed TPs + phantom empty-ref ---
todel=[]
for f in b.GetFootprints():
    ref=f.GetReference()
    if ref in ('TP1','TP4','TP5','TP6'): todel.append((ref,f))
    elif ref=='' and f.GetFPIDAsString()=='': todel.append(('<phantom>',f))
for name,f in todel: print("delete footprint:",name)
# TP6's LED_DATA5 stub becomes dangling once TP6 is gone -> delete it too
# (TP6 pad was (152.0,121.0); stub ran to D10.2 (153.639,124.068))
def near(p,x,y,t=0.02): return abs(U(p.x)-x)<t and abs(U(p.y)-y)<t
stub_del=[]
for t in b.GetTracks():
    if t.Type()==pcbnew.PCB_VIA_T or t.GetNetname()!='/LED_DATA5': continue
    s,e=t.GetStart(),t.GetEnd()
    if near(s,152.0,121.0) or near(e,152.0,121.0): stub_del.append(t)
print("delete TP6 LED_DATA5 stub segments:",len(stub_del))
# Remove LAST (Remove() corrupts later lookups)
for name,f in todel: b.Remove(f)
for t in stub_del: b.Remove(t)

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save('airwriter.kicad_pcb')
print("saved airwriter.kicad_pcb")
