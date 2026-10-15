"""Can the board get into the tray? Sweeps the board (slab, J1/J2/J4 STEP hulls,
S1's keys and body) along a rock-in path and reports any overlap with the tray.

    cd enclosure && .venv/bin/python ../docs/enclosure-build-log/flex-wall/sweep.py

Path, all rotations about X (back edge up): lower the board 3 mm behind its
home at TH0 about pivot A, slide forward so the mini-DIN tops tuck under their
lintels, rock down to TH1 about A, slide over to pivot B's pose, rock flat
about B. The flexed west wall is approximated by clearing the rim 2.0 mm out
over the keys' path (FEA: flex-wall-report.md)."""
import math
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "enclosure"))
from build123d import Box, ConvexPolyhedron, Pos, Rot  # noqa: E402
import board_geom as bg  # noqa: E402
import case  # noqa: E402
import params as p  # noqa: E402


def board_parts():
    def hull(ref):
        # Front (y <= 0) and back hulled apart: one hull over the whole body
        # reaches from the mating face down to the pins and would cut the
        # windows' bottoms, which are built from the front part alone.
        s = case.connector_solid(ref)
        out = None
        for y in (-10, 10):
            v, _ = (s & (Pos(0, y, 0) * Box(200, 20, 100))).tessellate(0.05)
            h = ConvexPolyhedron([tuple(x) for x in v])
            out = h if out is None else out + h
        return out
    parts = {"slab": case._rounded_box(p.BOARD_W, p.BOARD_L, p.PCB, p.BOARD_W / 2,
                                        p.BOARD_L / 2, -p.PCB / 2, p.CORNER_R)}
    for r in ("J1", "J2", "J4"):
        parts[r] = hull(r)
    fp = bg.footprint("S1")
    xs = [lx for lx, _ in bg.pad_positions("S1")]
    ys = [bg.to_case(*bg.local_to_board(fp, lx, 0.0))[1] for lx in (min(xs), max(xs))]
    y0, y1 = min(ys) - p.DIP_KEY_HALF_WIDTH, max(ys) + p.DIP_KEY_HALF_WIDTH
    # keys + levers past the body face (+0.2) to their tips (-2.2), z 5.6-9.01
    parts["S1keys"] = Pos(-1.0, (y0 + y1) / 2, (5.6 + 9.01) / 2) * Box(2.4, y1 - y0, 9.01 - 5.6)
    parts["S1body"] = Pos(5.0, (y0 + y1) / 2, 9.41 / 2) * Box(9.6, y1 - y0 + 1.0, 9.41)
    return parts


def check(tray, parts, P, a, dy=0.0, dz=0.0):
    out = []
    for n, s in parts.items():
        m = Pos(0, dy, dz) * Pos(0, *P) * Rot(a, 0, 0) * Pos(0, -P[0], -P[1]) * s
        x = m & tray
        if x is not None and x.volume > 1e-3:
            bb = x.bounding_box()
            out.append(f"{n}({x.volume:.1f}mm3 y{bb.min.Y:.1f}..{bb.max.Y:.1f} z{bb.min.Z:.1f}..{bb.max.Z:.1f})")
    return out


def offset(A, B, a):
    """Pose about A minus pose about B at the same angle: (I - R)(A - B)."""
    t = math.radians(a)
    dy, dz = A[0] - B[0], A[1] - B[1]
    return ((1 - math.cos(t)) * dy + math.sin(t) * dz, -math.sin(t) * dy + (1 - math.cos(t)) * dz)


def rock_in(tray, parts, A, B, th0, th1):
    res = []
    for dz in (25, 12, 6, 3, 0):
        h = check(tray, parts, A, th0, dy=3.0, dz=dz); h and res.append(f"lower dz{dz}: {' '.join(h)}")
    for dy in (2.0, 1.0, 0.0):
        h = check(tray, parts, A, th0, dy=dy); h and res.append(f"tuck dy{dy}: {' '.join(h)}")
    a = th0
    while a >= th1:
        h = check(tray, parts, A, a); h and res.append(f"A {a}deg: {' '.join(h)}"); a -= 2.5
    oy, oz = offset(A, B, th1)
    for f in (0.75, 0.5, 0.25, 0.0):
        h = check(tray, parts, B, th1, oy * f, oz * f); h and res.append(f"slide {f}: {' '.join(h)}")
    a = th1
    while a >= 0:
        h = check(tray, parts, B, a); h and res.append(f"B {a}deg: {' '.join(h)}"); a -= 2.5
    return res


if __name__ == "__main__":
    parts = board_parts()
    tray = case.tray()
    print("flat, at home:", check(tray, parts, (0, 0), 0) or "CLEAR")
    x0 = -p.POCKET_CLEAR - 2.0
    bow = Pos((x0 + 1) / 2, (20.0 + 40.5) / 2, (p.DIP_SLOT_Z1 + 16) / 2) * Box(1 - x0, 20.5, 16 - p.DIP_SLOT_Z1)
    for label, t in (("rigid", tray), ("rim bowed 2.0mm", tray - bow)):
        res = rock_in(t, parts, (0.0, 12.0), (1.0, 4.5), 45, 10)
        print(f"{label}: " + ("CLEAR" if not res else "\n  " + "\n  ".join(res)))
