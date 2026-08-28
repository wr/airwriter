import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, serialize, unquote

SCH = 'hardware/airwriter/airwriter.kicad_sch'
tree = parse(open(SCH).read())
root = tree[0]

DOOMED_REFS = {'SW2', 'R20', 'C19', 'D1', 'C8', 'D2', 'C20', 'D4', 'D5', 'D6'}

DOOMED_WIRE_PAIRS = [
    ((283.21, 105.41), (285.75, 105.41)),
    ((142.24, 223.52), (139.7, 223.52)),
    ((149.86, 215.9), (149.86, 213.36)),
    ((149.86, 231.14), (149.86, 233.68)),
    ((176.53, 219.71), (176.53, 217.17)),
    ((176.53, 227.33), (176.53, 229.87)),
    ((144.78, 195.58), (142.24, 195.58)),
    ((154.94, 195.58), (157.48, 195.58)),
    ((162.56, 191.77), (162.56, 189.23)),
    ((162.56, 199.39), (162.56, 201.93)),
]

DOOMED_NC = {(85.09, 115.57)}

DOOMED_LABEL_COORDS = {
    (157.48, 223.52), (275.59, 97.79), (299.72, 101.6), (299.72, 109.22),
    (275.59, 113.03), (267.97, 105.41), (285.75, 105.41), (139.7, 223.52),
    (149.86, 213.36), (149.86, 233.68), (176.53, 217.17), (176.53, 229.87),
    (142.24, 195.58), (157.48, 195.58), (162.56, 189.23), (162.56, 201.93),
    (33.02, 107.95), (33.02, 123.19), (25.4, 115.57), (40.64, 115.57),
    (54.61, 107.95), (54.61, 123.19), (46.99, 115.57), (62.23, 115.57),
    (77.47, 107.95), (77.47, 123.19), (69.85, 115.57),
    (33.02, 129.54), (33.02, 137.16),
}


def sym_ref(sym):
    for c in sym:
        if isinstance(c, list) and str(c[0]) == 'property' and unquote(c[1]) == 'Reference':
            return unquote(c[2])
    return None


def pt_eq(a, b, eps=1e-6):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


new_root = []
removed_syms = []
removed_wires = []
removed_ncs = []
removed_labels = []

for node in root:
    if isinstance(node, list) and node and str(node[0]) == 'symbol':
        ref = sym_ref(node)
        if ref in DOOMED_REFS:
            removed_syms.append(ref)
            continue
        new_root.append(node)
        continue

    if isinstance(node, list) and node and str(node[0]) == 'wire':
        pts_node = [c for c in node if isinstance(c, list) and str(c[0]) == 'pts'][0]
        pts = [(float(xy[1]), float(xy[2])) for xy in pts_node[1:]]
        assert len(pts) == 2
        matched = False
        for pair in DOOMED_WIRE_PAIRS:
            if (pt_eq(pts[0], pair[0]) and pt_eq(pts[1], pair[1])) or \
               (pt_eq(pts[0], pair[1]) and pt_eq(pts[1], pair[0])):
                matched = True
                break
        if matched:
            removed_wires.append(pts)
            continue
        new_root.append(node)
        continue

    if isinstance(node, list) and node and str(node[0]) == 'no_connect':
        at = [c for c in node if isinstance(c, list) and str(c[0]) == 'at'][0]
        p = (float(at[1]), float(at[2]))
        if any(pt_eq(p, d) for d in DOOMED_NC):
            removed_ncs.append(p)
            continue
        new_root.append(node)
        continue

    if isinstance(node, list) and node and str(node[0]) == 'label':
        at = [c for c in node if isinstance(c, list) and str(c[0]) == 'at'][0]
        p = (float(at[1]), float(at[2]))
        if any(pt_eq(p, d) for d in DOOMED_LABEL_COORDS):
            removed_labels.append((unquote(node[1]), p))
            continue
        new_root.append(node)
        continue

    new_root.append(node)

root[:] = new_root

assert set(removed_syms) == DOOMED_REFS, (removed_syms, DOOMED_REFS)
assert len(removed_wires) == len(DOOMED_WIRE_PAIRS), removed_wires
assert len(removed_ncs) == len(DOOMED_NC), removed_ncs
assert len(removed_labels) == len(DOOMED_LABEL_COORDS), (len(removed_labels), len(DOOMED_LABEL_COORDS))

out = serialize(tree[0]) + '\n'
open(SCH, 'w').write(out)

print("Removed symbols:", sorted(removed_syms))
print("Removed wires:", len(removed_wires))
print("Removed no_connects:", len(removed_ncs))
print("Removed labels:", len(removed_labels))
