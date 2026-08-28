import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, unquote

SCH = 'hardware/airwriter/airwriter.kicad_sch'
tree = parse(open(SCH).read())
root = tree[0]

def find_all(node, tag):
    out = []
    if isinstance(node, list) and node and str(node[0]) == tag:
        out.append(node)
    if isinstance(node, list):
        for c in node:
            out.extend(find_all(c, tag))
    return out

lib_symbols = [n for n in root if isinstance(n, list) and str(n[0]) == 'lib_symbols'][0]

def lib_pins(lib_id):
    for sym in lib_symbols[1:]:
        if isinstance(sym, list) and str(sym[0]) == 'symbol' and unquote(sym[1]) == lib_id:
            pins = find_all(sym, 'pin')
            out = []
            for p in pins:
                at = [c for c in p if isinstance(c, list) and str(c[0]) == 'at'][0]
                number = [c for c in p if isinstance(c, list) and str(c[0]) == 'number'][0]
                out.append((unquote(number[1]), float(at[1]), float(at[2])))
            return out
    raise KeyError(lib_id)

def transform(px, py, rot):
    rot = int(rot) % 360
    if rot == 0:
        x, y = px, py
    elif rot == 90:
        x, y = -py, px
    elif rot == 180:
        x, y = -px, -py
    elif rot == 270:
        x, y = py, -px
    else:
        raise ValueError(rot)
    return (x, -y)

def top_symbols(root):
    return [n for n in root if isinstance(n, list) and str(n[0]) == 'symbol']

def sym_ref(sym):
    for c in sym:
        if isinstance(c, list) and str(c[0]) == 'property' and unquote(c[1]) == 'Reference':
            return unquote(c[2])
    return None

def sym_lib_id(sym):
    return unquote([c for c in sym if isinstance(c, list) and str(c[0]) == 'lib_id'][0][1])

def sym_placement(sym):
    at = [c for c in sym if isinstance(c, list) and str(c[0]) == 'at'][0]
    rot = at[3] if len(at) > 3 else '0'
    return float(at[1]), float(at[2]), rot

def sym_abs_pins(sym):
    lib_id = sym_lib_id(sym)
    ox, oy, rot = sym_placement(sym)
    out = []
    for number, px, py in lib_pins(lib_id):
        dx, dy = transform(px, py, rot)
        out.append((number, round(ox + dx, 4), round(oy + dy, 4)))
    return out

DOOMED_REFS = {'SW2', 'R20', 'C19', 'D1', 'C8', 'D2', 'C20', 'D4', 'D5', 'D6'}

all_syms = top_symbols(root)
doomed_syms = [s for s in all_syms if sym_ref(s) in DOOMED_REFS]
found_refs = {sym_ref(s) for s in doomed_syms}
missing = DOOMED_REFS - found_refs
if missing:
    print("MISSING REFS:", missing)

doomed_coords = set()
pin_report = {}
for s in doomed_syms:
    ref = sym_ref(s)
    pins = sym_abs_pins(s)
    pin_report[ref] = pins
    for number, x, y in pins:
        doomed_coords.add((x, y))

print("=== doomed symbol pins ===")
for ref, pins in pin_report.items():
    print(ref, pins)

# wire propagation
wires = find_all(root, 'wire')
def wire_pts(w):
    pts = [c for c in w if isinstance(c, list) and str(c[0]) == 'pts'][0]
    return [(round(float(xy[1]),4), round(float(xy[2]),4)) for xy in pts[1:]]

removed_wires = []
changed = True
while changed:
    changed = False
    for w in wires:
        if w in removed_wires:
            continue
        pts = wire_pts(w)
        if any(p in doomed_coords for p in pts):
            removed_wires.append(w)
            for p in pts:
                if p not in doomed_coords:
                    doomed_coords.add(p)
                    changed = True

print("\n=== removed wires ===")
for w in removed_wires:
    print(wire_pts(w))

# no_connects at doomed coords
no_connects = find_all(root, 'no_connect')
removed_ncs = []
for nc in no_connects:
    at = [c for c in nc if isinstance(c, list) and str(c[0]) == 'at'][0]
    p = (round(float(at[1]),4), round(float(at[2]),4))
    if p in doomed_coords:
        removed_ncs.append(nc)
print("\n=== removed no_connects ===")
for nc in removed_ncs:
    print(nc)

# labels at doomed coords
labels = find_all(root, 'label')
removed_labels = []
for l in labels:
    at = [c for c in l if isinstance(c, list) and str(c[0]) == 'at'][0]
    p = (round(float(at[1]),4), round(float(at[2]),4))
    if p in doomed_coords:
        removed_labels.append(l)
print("\n=== removed labels ===")
for l in removed_labels:
    at = [c for c in l if isinstance(c, list) and str(c[0]) == 'at'][0]
    print(unquote(l[1]), at[1], at[2])

# safety cross-check: any surviving symbol pin in doomed_coords?
survivors = [s for s in all_syms if sym_ref(s) not in DOOMED_REFS]
conflicts = []
for s in survivors:
    for number, x, y in sym_abs_pins(s):
        if (x, y) in doomed_coords:
            conflicts.append((sym_ref(s), number, x, y))
print("\n=== CONFLICTS (surviving symbol pins inside doomed_coords) ===")
for c in conflicts:
    print(c)

print("\nTotal doomed_coords:", len(doomed_coords))
print("Total wires to remove:", len(removed_wires))
print("Total no_connects to remove:", len(removed_ncs))
print("Total labels to remove:", len(removed_labels))
