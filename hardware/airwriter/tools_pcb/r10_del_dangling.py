#!/usr/bin/env python3
"""Delete /VBUS track segments flagged track_dangling in a DRC json, then refill.
Usage: r10_del_dangling.py <drc.json>  (deletes only the flagged dangling UUIDs)"""
import sys, json, pcbnew
b=pcbnew.LoadBoard('airwriter.kicad_pcb')
U=lambda v:round(v/1e6,3)
d=json.load(open(sys.argv[1]))
uuids={v['items'][0]['uuid'] for v in d['violations'] if v['type']=='track_dangling'}
print("dangling uuids:",len(uuids))
todel=[t for t in b.GetTracks() if t.Type()!=pcbnew.PCB_VIA_T and t.m_Uuid.AsString() in uuids]
for t in todel:
    print(f"  del ({U(t.GetStart().x)},{U(t.GetStart().y)})-({U(t.GetEnd().x)},{U(t.GetEnd().y)}) net {t.GetNetname()}")
for t in todel:
    b.Remove(t)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save('airwriter.kicad_pcb')
print("removed",len(todel),"and saved")
