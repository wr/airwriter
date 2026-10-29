#!/usr/bin/env python3
"""Rev 2026.11: remove rev A copper the new schematic makes wrong.

    <KiCad python> tools_pcb/r13_clean.py

Run after r13_sync.py. Two passes, printing everything it removes:

  1. a track or via that touches a pad of another net (the copper that went
     to U2's receive pins, U7.4, U1.33, U3.8 and U3.10's GND vias, D3's old
     pads) goes;
  2. then, repeatedly, any track end or via left connected to nothing of its
     own net (no pad, track, via or zone) goes, so a chain cut in pass 1 is
     taken back to the pad it started from rather than left as a stub.

Wells redraws what this removes. Close KiCad first. Safe to run twice: on a
clean board it removes nothing.
"""
import os

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')
COPPER = (pcbnew.F_Cu, pcbnew.B_Cu)
# Nets whose pads rev 2026.11 changed: their stubs may be cut on a re-run too.
# Rev A routes kept although cut short: r13_reroute.py joins them back up.
KEEP = {'/LT_RESET',     # U1.38 to U7.4 along the back; R27 now sits in it
        '/UART_CTS', '/PR_HSKO', '/HSKI_RS232',   # U3's channel swap moves their pins
        '/EN'}           # SW1's bracket pad cuts its last leg; r13_reroute.py redraws it
# The DIP switches are gone, so all their copper goes; IO35 (HSKI_DRV) moves to
# IO47, so rev A's run from it goes too and r13_route.py draws the new one.
DIPS = {f'/DIP{n}' for n in range(1, 9)}
NEW_NETS = {'/LT_RESET', '/LT_MCLR', '/PR_TXD_P', '/PR_TXD_N',
            '/PR_TXD_P_IN', '/PR_TXD_N_IN', '/J4_HSKI', 'Net-(D3-K)', '/+3V3',
            '/UART_CTS', '/PR_HSKO', '/HSKI_RS232', '/HSKI_DRV', '/EN'} | DIPS


def mm(v):
    return round(pcbnew.ToMM(v), 3)


def describe(t):
    kind = 'via' if isinstance(t, pcbnew.PCB_VIA) else 'track'
    if kind == 'via':
        p = t.GetPosition()
        return f'via {t.GetNetname()} at ({mm(p.x)}, {mm(p.y)})'
    s, e = t.GetStart(), t.GetEnd()
    return (f'track {t.GetNetname()} {t.GetLayerName()} ({mm(s.x)}, {mm(s.y)})-'
            f'({mm(e.x)}, {mm(e.y)})')


def layers_of(t):
    return COPPER if isinstance(t, pcbnew.PCB_VIA) else (t.GetLayer(),)


def ends_of(t):
    if isinstance(t, pcbnew.PCB_VIA):
        return [t.GetPosition()]
    return [t.GetStart(), t.GetEnd()]


def on_line(t, pos, tol=1000):
    """True when pos lies on track t's centreline (within 1 um): its ends or a
    T-junction. HitTest with the track's width would count a near miss."""
    s, e = t.GetStart(), t.GetEnd()
    dx, dy = e.x - s.x, e.y - s.y
    L2 = dx * dx + dy * dy
    u = 0 if L2 == 0 else max(0.0, min(1.0, ((pos.x - s.x) * dx + (pos.y - s.y) * dy) / L2))
    return ((s.x + u * dx - pos.x) ** 2 + (s.y + u * dy - pos.y) ** 2) <= tol * tol


def pads_at(board, pos, layer):
    return [p for fp in board.GetFootprints() for p in fp.Pads()
            if p.IsOnLayer(layer) and p.HitTest(pos)]


def wrong_net(board):
    """Copper whose shape meets a pad of another net, anywhere along it: a
    track running under a part that moved there (D3) as well as one ending on
    a pad whose net changed."""
    pads = [p for fp in board.GetFootprints() for p in fp.Pads()]
    out = []
    for t in board.GetTracks():
        bad = {p.GetParentFootprint().GetReference() + '.' + p.GetNumber()
               for layer in layers_of(t) for p in pads
               if p.IsOnLayer(layer) and p.GetNetCode() != t.GetNetCode()
               and p.GetEffectiveShape(layer).Collide(t.GetEffectiveShape(layer), 0)}
        if bad:
            out.append((t, 'wrong net', 'touches ' + ', '.join(sorted(bad))))
    return out


def dangling(board, nets):
    tracks = list(board.GetTracks())
    zones = list(board.Zones())
    out = []
    def joined(t, pos, layer):
        return (any(p.GetNetCode() == t.GetNetCode() for p in pads_at(board, pos, layer))
                or any(o is not t and o.GetNetCode() == t.GetNetCode() and layer in layers_of(o)
                       and (o.HitTest(pos) if isinstance(o, pcbnew.PCB_VIA) else on_line(o, pos))
                       for o in tracks)
                or any(z.GetNetCode() == t.GetNetCode() and z.IsOnLayer(layer)
                       and z.HitTestFilledArea(layer, pos) for z in zones))

    def via_joined(v, layer):
        # a track joins a via when one of its ends lands anywhere in the via, but a
        # stub lying wholly inside the via joins nothing: rev A's DIP and IO35 runs
        # end so, and the stub and via would otherwise hold each other up
        pos = v.GetPosition()
        if any(p.GetNetCode() == v.GetNetCode() for p in pads_at(board, pos, layer)):
            return True
        if any(z.GetNetCode() == v.GetNetCode() and z.IsOnLayer(layer)
               and z.HitTestFilledArea(layer, pos) for z in zones):
            return True
        for o in tracks:
            if o is v or o.GetNetCode() != v.GetNetCode():
                continue
            if isinstance(o, pcbnew.PCB_VIA):
                if o.HitTest(pos):
                    return True
                continue
            if o.GetLayer() != layer:
                continue
            s, e = o.GetStart(), o.GetEnd()
            if abs(s.x - e.x) + abs(s.y - e.y) > 10000 and all(v.HitTest(p) for p in (s, e)):
                continue        # (rev A's near-zero pieces, 0.1 um, mark a via's landing: they count)
            if on_line(o, pos) or any(v.HitTest(e) for e in ends_of(o)):
                return True
        return False

    for t in tracks:
        if t.GetNetname() in KEEP:
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            # a via earns its keep only by joining two layers
            if sum(via_joined(t, layer) for layer in COPPER) < 2:
                out.append((t, 'dangling', ''))
        elif not all(joined(t, pos, t.GetLayer()) for pos in ends_of(t)):
            out.append((t, 'dangling', ''))
    # Stubs left inside a pad once the track they led to is gone: a cluster of
    # same-net segments lying wholly inside one pad that nothing outside the
    # cluster touches. Rev A's pad-entry stubs still meet their next segment.
    # Only on nets this script has cut, so rev A's other copper stays as it was.
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() not in nets:
                continue
            inside = [t for t in tracks if not isinstance(t, pcbnew.PCB_VIA)
                      and t.GetNetCode() == pad.GetNetCode() and pad.IsOnLayer(t.GetLayer())
                      and pad.HitTest(t.GetStart()) and pad.HitTest(t.GetEnd())]
            if not inside:
                continue
            ends = [e for t in inside for e in ends_of(t)]
            outside = [o for o in tracks if o not in inside and o.GetNetCode() == pad.GetNetCode()]
            if not any(o.HitTest(e) if isinstance(o, pcbnew.PCB_VIA) else on_line(o, e)
                       for o in outside for e in ends):
                out += [(t, 'dangling', '(stub inside ' + fp.GetReference() + '.'
                         + pad.GetNumber() + ')') for t in inside if t not in [x for x, _, _ in out]]
    return out


def run(finder, nets):
    """One pass on a freshly loaded board (SWIG's track list goes stale after a
    Remove, so each pass loads, removes, saves). Adds the nets it cut to
    `nets`; returns how many items it removed."""
    board = pcbnew.LoadBoard(BOARD)
    found = finder(board, nets)
    for t, why, more in found:
        print(why, describe(t), more)
        nets.add(t.GetNetname())
        board.Delete(t)
    if found:
        board.Save(BOARD)
    return len(found)


def main():
    board = pcbnew.LoadBoard(BOARD)
    strays = [(t, more) for t, _, more in wrong_net(board) if t.GetNetname() not in NEW_NETS]
    if strays:
        # copper on a net rev 2026.11 didn't touch meets a pad: a part was placed
        # on a live track. Move the part; never cut the track.
        for t, more in strays:
            print('PLACEMENT ERROR', describe(t), more)
        raise SystemExit(1)
    nets = set(NEW_NETS)
    removed = run(lambda b, n: wrong_net(b), nets)
    while True:
        n = run(dangling, nets)
        if not n:
            break
        removed += n
    print(f'{removed} items removed')


if __name__ == '__main__':
    main()
