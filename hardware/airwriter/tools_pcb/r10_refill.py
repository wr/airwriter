#!/usr/bin/env python3
"""Refill all zones via ZONE_FILLER (writes back to file) and save."""
import sys
import pcbnew
BOARD='airwriter.kicad_pcb'
b=pcbnew.LoadBoard(BOARD)
filler=pcbnew.ZONE_FILLER(b)
filler.Fill(b.Zones())
b.Save(BOARD)
print("refilled",len(list(b.Zones())),"zones and saved")
