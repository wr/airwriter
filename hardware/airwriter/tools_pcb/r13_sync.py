#!/usr/bin/env python3
"""Rev 2026.11: bring the board in line with the schematic, headless.

    kicad-cli sch export netlist --format kicadxml -o net.xml airwriter.kicad_sch
    <KiCad python> tools_pcb/r13_sync.py net.xml

KiCad's own update (and konnect's) refuses this board: rev A's 37 LEDs and SW1
were placed by script with an empty schematic path, so they read as strangers.
This does what Update PCB from Schematic would, matched by reference:

  - every footprint the schematic has gets its symbol's path (the LEDs and SW1
    included, so a later F8 in the GUI matches them instead of replacing them);
  - footprints the schematic no longer has are removed (board-only ones, with
    no symbol by design -- holes, test pads -- are left alone);
  - new symbols get a footprint from KiCad's library, parked off the board's
    east edge for r13_place.py; a changed footprint (D3) is swapped in place;
  - values and the LCSC/MPN/Note fields follow the schematic;
  - every pad takes its net from the netlist, and copper on a net that was
    only renamed (the LocalTalk pairs) follows the rename.

Copper that now joins two nets is left for r13_place.py to remove. Close
KiCad first.
"""
import os
import sys
import xml.etree.ElementTree as ET

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')
FP_ROOT = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints'
BOARD_ONLY = ('H', 'TP')                      # refs with no symbol, by design
RENAMED = {                                   # rev A net -> rev 2026.11 net
    '/LT_A': '/LT_RXD_N', '/LT_B': '/LT_RXD_P', '/LT_Y': '/LT_TXD_N', '/LT_Z': '/LT_TXD_P',
    '/LT_PORT_A': '/LT_PORT_RXD_N', '/LT_PORT_B': '/LT_PORT_RXD_P',
    '/LT_PORT_Y': '/LT_PORT_TXD_N', '/LT_PORT_Z': '/LT_PORT_TXD_P',
}
FIELDS = ('LCSC', 'MPN', 'Note')
PARK_X, PARK_Y0 = 170.0, 70.0                 # east of the 162.05 edge


def load_netlist(path):
    root = ET.parse(path).getroot()
    comps = {}
    for c in root.iter('comp'):
        fields = {f.get('name'): (f.text or '') for f in c.iter('field')}
        comps[c.get('ref')] = dict(value=c.findtext('value'), fpid=c.findtext('footprint'),
                                   uuid=c.findtext('tstamps').strip(), fields=fields)
    pins = {}
    for n in root.iter('net'):
        for node in n.iter('node'):
            pins[(node.get('ref'), node.get('pin'))] = n.get('name')
    return comps, pins


def library_footprint(fpid):
    lib, name = fpid.split(':')
    path = os.path.join(FP_ROOT, lib + '.pretty')
    if not os.path.isdir(path):
        path = os.path.join(HERE, '..', 'lib', lib + '.pretty')
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        sys.exit(f'no footprint {fpid}')
    fp.SetFPIDAsString(fpid)
    return fp


def net(board, name):
    n = board.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
    return n


def main():
    comps, pins = load_netlist(sys.argv[1])
    board = pcbnew.LoadBoard(BOARD)
    on_board = {fp.GetReference(): fp for fp in board.GetFootprints()}

    for ref, fp in sorted(on_board.items()):
        if ref not in comps and not ref.startswith(BOARD_ONLY):
            board.Remove(fp)
            print('removed', ref)

    parked = 0
    for ref, c in sorted(comps.items()):
        fp = on_board.get(ref)
        if fp is not None and fp.GetFPIDAsString() != c['fpid']:
            new = library_footprint(c['fpid'])
            new.SetPosition(fp.GetPosition())
            new.SetOrientation(fp.GetOrientation())
            board.Remove(fp)
            board.Add(new)
            print('swapped', ref, fp.GetFPIDAsString(), '->', c['fpid'])
            fp = new
        elif fp is None:
            fp = library_footprint(c['fpid'])
            fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(PARK_X), pcbnew.FromMM(PARK_Y0 + 3 * parked)))
            board.Add(fp)
            parked += 1
            print('added', ref, c['fpid'])
        fp.SetReference(ref)
        fp.SetValue(c['value'])
        fp.SetPath(pcbnew.KIID_PATH('/' + c['uuid']))
        for name in FIELDS:
            if c['fields'].get(name):
                fp.SetField(name, c['fields'][name])
                f = fp.GetField(name)            # data, not artwork: hidden, on F.Fab
                f.SetVisible(False)
                f.SetLayer(pcbnew.F_Fab)

    for t in board.GetTracks():
        new = RENAMED.get(t.GetNetname())
        if new:
            t.SetNet(net(board, new))
    for z in board.Zones():
        new = RENAMED.get(z.GetNetname())
        if new:
            z.SetNet(net(board, new))

    changed = 0
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref not in comps:
            continue
        for pad in fp.Pads():
            want = pins.get((ref, pad.GetNumber()))
            if want and pad.GetNetname() != want:
                print(f'  {ref}.{pad.GetNumber()}: {pad.GetNetname() or "-"} -> {want}')
                pad.SetNet(net(board, want))
                changed += 1
    print(f'{changed} pads changed net')
    board.Save(BOARD)


if __name__ == '__main__':
    main()
