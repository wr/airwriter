import os
import sys
import uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, serialize, unquote, Sym

SCH = 'hardware/airwriter/airwriter.kicad_sch'
tree = parse(open(SCH).read())
root = tree[0]

DOOMED_REFS = {'J6', 'R18', 'R19', 'J3'}

DOOMED_WIRE_PAIRS = [
    ((190.5, 251.46), (187.96, 251.46)),
    ((190.5, 254.0), (187.96, 254.0)),
    ((190.5, 256.54), (187.96, 256.54)),
    ((190.5, 259.08), (187.96, 259.08)),
    ((173.99, 241.3), (173.99, 238.76)),
    ((173.99, 248.92), (173.99, 251.46)),
    ((184.15, 241.3), (184.15, 238.76)),
    ((184.15, 248.92), (184.15, 251.46)),
    # U1 IO36/IO37 pin-to-label stubs (I2C dies, these pins go no-connect)
    ((219.71, 137.16), (222.25, 137.16)),
    ((219.71, 139.7), (222.25, 139.7)),
]

DOOMED_LABEL_COORDS = {
    (270.51, 172.72), (270.51, 180.34), (270.51, 175.26), (270.51, 177.8),
    (187.96, 251.46), (187.96, 254.0), (187.96, 256.54), (187.96, 259.08),
    (173.99, 238.76), (173.99, 251.46), (184.15, 238.76), (184.15, 251.46),
    # U1's own I2C_SDA/I2C_SCL pin labels (IO36/IO37) -- net dies entirely
    (222.25, 137.16), (222.25, 139.7),
}

NEW_NO_CONNECTS = [(219.71, 137.16), (219.71, 139.7)]


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
removed_labels = []
sw1_footprint_changed = False

for node in root:
    if isinstance(node, list) and node and str(node[0]) == 'symbol':
        ref = sym_ref(node)
        if ref in DOOMED_REFS:
            removed_syms.append(ref)
            continue
        if ref == 'SW1':
            for c in node:
                if isinstance(c, list) and str(c[0]) == 'property' and unquote(c[1]) == 'Footprint':
                    assert unquote(c[2]) == 'airwriter:BootPads_Service', c[2]
                    c[2] = '"airwriter:EVQP44B3M_SidePress"'
                    sw1_footprint_changed = True
                if isinstance(c, list) and str(c[0]) == 'property' and unquote(c[1]) == 'Value':
                    c[2] = '"EVQP44B3M side-press"'
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
assert len(removed_wires) == len(DOOMED_WIRE_PAIRS), (len(removed_wires), removed_wires)
assert len(removed_labels) == len(DOOMED_LABEL_COORDS), (len(removed_labels), DOOMED_LABEL_COORDS)
assert sw1_footprint_changed

# Add the two new no_connects for U1's freed IO36/IO37 pins.
insert_idx = None
for i, node in enumerate(root):
    if isinstance(node, list) and node and str(node[0]) == 'no_connect':
        insert_idx = i
for x, y in NEW_NO_CONNECTS:
    nc = [Sym('no_connect'),
          [Sym('at'), Sym(str(x)), Sym(str(y))],
          [Sym('uuid'), '"%s"' % uuid.uuid4()]]
    root.insert(insert_idx + 1, nc)
    insert_idx += 1

out = serialize(tree[0]) + '\n'
open(SCH, 'w').write(out)

print("Removed symbols:", sorted(removed_syms))
print("Removed wires:", len(removed_wires))
print("Removed labels:", len(removed_labels))
print("SW1 footprint changed:", sw1_footprint_changed)
print("Added no_connects:", NEW_NO_CONNECTS)
