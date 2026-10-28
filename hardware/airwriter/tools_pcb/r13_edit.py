"""Small helpers the rev 2026.11 scripts share for exact board edits (KiCad python).

Every edit names its copper by net and end points, so a script fails loudly when
the board is not the state it was written against instead of editing the wrong
thing.
"""
import pcbnew

F, B = pcbnew.F_Cu, pcbnew.B_Cu


def nm(v):
    return int(round(v * 1e6))


def mm(v):
    return v / 1e6


def near(a, b, tol=0.02):
    return abs(a[0] - b[0]) < tol and abs(a[1] - b[1]) < tol


class Board:
    def __init__(self, path):
        self.path = path
        self.b = pcbnew.LoadBoard(path)
        self.dead = []

    def net(self, name):
        n = self.b.FindNet(name)
        if n is None:
            raise SystemExit(f'no net {name}')
        return n

    def track(self, net, a, c):
        for t in self.b.GetTracks():
            if isinstance(t, pcbnew.PCB_VIA) or t.GetNetname() != net:
                continue
            s, e = (mm(t.GetStart().x), mm(t.GetStart().y)), (mm(t.GetEnd().x), mm(t.GetEnd().y))
            if (near(s, a) and near(e, c)) or (near(s, c) and near(e, a)):
                return t
        raise SystemExit(f'missing track {net} {a}-{c}')

    def via(self, net, p):
        for t in self.b.GetTracks():
            if (isinstance(t, pcbnew.PCB_VIA) and t.GetNetname() == net
                    and near((mm(t.GetPosition().x), mm(t.GetPosition().y)), p, 0.01)):
                return t
        raise SystemExit(f'missing via {net} {p}')

    def drop(self, *items):
        self.dead.extend(items)

    def move_end(self, t, old, new):
        s = t.GetStart()
        if near((mm(s.x), mm(s.y)), old):
            t.SetStart(pcbnew.VECTOR2I(nm(new[0]), nm(new[1])))
        else:
            t.SetEnd(pcbnew.VECTOR2I(nm(new[0]), nm(new[1])))

    def draw(self, net, pts, layer=F, width=0.2):
        for a, c in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(pcbnew.VECTOR2I(nm(a[0]), nm(a[1])))
            t.SetEnd(pcbnew.VECTOR2I(nm(c[0]), nm(c[1])))
            t.SetWidth(nm(width)); t.SetLayer(layer); t.SetNet(self.net(net))
            self.b.Add(t)

    def add_via(self, net, p, size=0.6, drill=0.3):
        v = pcbnew.PCB_VIA(self.b)
        v.SetPosition(pcbnew.VECTOR2I(nm(p[0]), nm(p[1])))
        v.SetWidth(nm(size)); v.SetDrill(nm(drill)); v.SetLayerPair(F, B)
        v.SetNet(self.net(net))
        self.b.Add(v)

    def place(self, ref, x, y, deg):
        fp = self.b.FindFootprintByReference(ref)
        fp.SetPosition(pcbnew.VECTOR2I(nm(x), nm(y)))
        fp.SetOrientationDegrees(deg)
        fp.Reference().SetPosition(fp.GetPosition())

    def save(self):
        for t in self.dead:
            self.b.Remove(t)
        pcbnew.ZONE_FILLER(self.b).Fill(self.b.Zones())
        self.b.Save(self.path)
