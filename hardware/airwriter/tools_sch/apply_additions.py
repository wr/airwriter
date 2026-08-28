import os, sys, uuid, copy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, serialize, unquote, quote, Sym

SCH = 'hardware/airwriter/airwriter.kicad_sch'
PROJECT_PATH = '/b907fa76-c5fe-4a7a-8e4e-adfdab62cb9c'

tree = parse(open(SCH).read())
root = tree[0]


def S(name):
    return Sym(name)


def L(*items):
    return list(items)


def new_uuid():
    return quote(str(uuid.uuid4()))


def g(n):
    """Grid-snap helper: n is a count of 1.27mm (50mil) grid steps. Every pin
    offset used below (7.62, 3.81, 10.16, 15.24, 12.7, 2.54, 5.08...) is itself
    an integer multiple of 1.27mm, so as long as every symbol's placement (x,y)
    is also chosen via g(), every absolute pin coordinate lands on-grid and ERC
    won't flag endpoint_off_grid warnings."""
    return round(n * 1.27, 2)


def at(x, y, rot=None):
    if rot is None:
        return L(S('at'), Sym(str(x)), Sym(str(y)))
    return L(S('at'), Sym(str(x)), Sym(str(y)), Sym(str(rot)))


def effects_simple():
    return L(S('effects'), L(S('font'), L(S('size'), Sym('1.27'), Sym('1.27'))))


def prop(name, value, x, y, rot=0, hide=False):
    node = [S('property'), quote(name), quote(value), at(x, y, rot)]
    if hide:
        node.append(L(S('hide'), S('yes')))
    node.append(effects_simple())
    return node


def instances_block(ref):
    return L(S('instances'),
             L(S('project'), quote('airwriter'),
               L(S('path'), quote(PROJECT_PATH),
                 L(S('reference'), quote(ref)),
                 L(S('unit'), Sym('1')))))


# ---------------------------------------------------------------------------
# 1. lib_symbols additions: Switch:SW_DIP_x08, 74xGxx:74AHCT1G125
# ---------------------------------------------------------------------------

lib_symbols = [n for n in root if isinstance(n, list) and str(n[0]) == 'lib_symbols'][0]


def rename_symbol_children(sym_node, old_base, new_base):
    """Recursively rename any string that is exactly old_base_X_Y to new_base_X_Y.
    NOTE: only ever used for old_base -> new_base within the *bare* (unprefixed)
    namespace, e.g. 74LVC1G125_0_1 -> 74AHCT1G125_0_1. KiCad's own convention
    (seen throughout this file, e.g. lib_symbols entry "LED:SK6812MINI-E" whose
    children are "SK6812MINI-E_0_0", NOT "LED:SK6812MINI-E_0_0") requires child
    unit/style symbol names to stay unprefixed even though the top-level entry
    name carries the "Library:" prefix. Prefixing the children breaks the KiCad
    schematic parser outright (silent "Failed to load schematic").
    """
    for i, c in enumerate(sym_node):
        if isinstance(c, str) and c.startswith('"') and c.endswith('"'):
            inner = unquote(c)
            if inner.startswith(old_base + '_'):
                sym_node[i] = quote(new_base + inner[len(old_base):])
        elif isinstance(c, list):
            rename_symbol_children(c, old_base, new_base)


sw_dip_lib = parse(open('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/Switch.kicad_sym').read())[0]
sw_dip_src = None
for s in sw_dip_lib[1:]:
    if isinstance(s, list) and str(s[0]) == 'symbol' and unquote(s[1]) == 'SW_DIP_x08':
        sw_dip_src = copy.deepcopy(s)
        break
assert sw_dip_src is not None
# Only the top-level entry name gets the library prefix; children keep their
# original bare unit/style names (SW_DIP_x08_0_0, etc.) per KiCad convention.
sw_dip_src[1] = quote('Switch:SW_DIP_x08')

lvc_lib = parse(open('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/74xGxx.kicad_sym').read())[0]
lvc_src = None
for s in lvc_lib[1:]:
    if isinstance(s, list) and str(s[0]) == 'symbol' and unquote(s[1]) == '74LVC1G125':
        lvc_src = copy.deepcopy(s)
        break
assert lvc_src is not None
# Children get renamed in the bare namespace (74LVC1G125_X_Y -> 74AHCT1G125_X_Y);
# only the top-level entry name is library-prefixed. See note above.
rename_symbol_children(lvc_src, '74LVC1G125', '74AHCT1G125')
lvc_src[1] = quote('74xGxx:74AHCT1G125')
# fix up properties to describe the AHCT part, not the LVC part
for c in lvc_src:
    if isinstance(c, list) and str(c[0]) == 'property':
        pname = unquote(c[1])
        if pname == 'Value':
            c[2] = quote('74AHCT1G125')
        elif pname == 'Datasheet':
            c[2] = quote('http://www.ti.com/lit/sg/scyt129e/scyt129e.pdf')
        elif pname == 'Description':
            c[2] = quote('Single Buffer Gate Tri-State, High-Speed CMOS (5V tolerant AHCT logic family)')
        elif pname == 'Footprint':
            c[2] = quote('')

lib_symbols.append(sw_dip_src)
lib_symbols.append(lvc_src)

print("lib_symbols added:", unquote(sw_dip_src[1]), unquote(lvc_src[1]))

# ---------------------------------------------------------------------------
# helpers for building instance-level nodes
# ---------------------------------------------------------------------------


def label(text, x, y, rot=0):
    return L(S('label'), quote(text), at(x, y, rot), L(S('uuid'), new_uuid()))


def no_connect(x, y):
    return L(S('no_connect'), at(x, y), L(S('uuid'), new_uuid()))


def build_symbol(lib_id, ref, x, y, rot, props):
    """props: list of (name, value, dx, dy, hide) tuples, dx/dy relative to x,y for text placement."""
    node = [S('symbol'),
            L(S('lib_id'), quote(lib_id)),
            at(x, y, rot),
            L(S('unit'), Sym('1')),
            L(S('in_bom'), S('yes')),
            L(S('on_board'), S('yes')),
            L(S('dnp'), S('no')),
            L(S('uuid'), new_uuid())]
    for (pname, pval, dx, dy, hide) in props:
        node.append(prop(pname, pval, x + dx, y + dy, 0, hide))
    node.append(instances_block(ref))
    return node


def led_props(ref):
    return [
        ('Reference', ref, 0, -3.81, False),
        ('Value', 'SK6812MINI-E', 0, 3.81, False),
        ('Footprint', 'LED_SMD:LED_SK6812MINI-E_3.2x2.8mm_P1.5mm', 0, 0, True),
        ('Datasheet', 'https://www.lcsc.com/datasheet/C5149201.pdf', 0, 0, True),
        ('MPN', 'SK6812MINI-E', 0, 0, True),
        ('Manufacturer', 'OPSCO Optoelectronics', 0, 0, True),
        ('Description', 'Addressable RGB LED, top firing, dot-panel array', 0, 0, True),
    ]


def cap_props(ref, value, footprint):
    return [
        ('Reference', ref, 0, -3.81, False),
        ('Value', value, 0, 3.81, False),
        ('Footprint', footprint, 0, 0, True),
        ('Datasheet', '', 0, 0, True),
    ]


# ---------------------------------------------------------------------------
# 2. U9 (74AHCT1G125 buffer) + C29
# ---------------------------------------------------------------------------

new_nodes = []  # top-level nodes (symbol/label/no_connect) to append to root

UX, UY = g(360), g(20)
u9_props = [
    ('Reference', 'U9', 0, -6.35, False),
    ('Value', '74AHCT1G125SE-7', 0, 12.7, False),
    ('Footprint', 'Package_TO_SOT_SMD:SOT-353_SC-70-5', 0, 0, True),
    ('Datasheet', 'http://www.ti.com/lit/sg/scyt129e/scyt129e.pdf', 0, 0, True),
    ('MPN', '74AHCT1G125SE-7', 0, 0, True),
    ('Manufacturer', 'Diodes Incorporated', 0, 0, True),
    ('Description', 'Single gate buffer, SOT-353. Levels LED_DATA up to a solid VBUS-referenced drive before the LED chain', 0, 0, True),
]
new_nodes.append(build_symbol('74xGxx:74AHCT1G125', 'U9', UX, UY, 0, u9_props))
# pins: 1=OE(bar) at local(0,10.16,270)->abs(x,y-10.16); 2=A local(-15.24,0,0)->abs(x-15.24,y)
# 3=GND local(-5.08,-10.16,90)->abs(x-5.08,y+10.16); 4=Y local(12.7,0,180)->abs(x+12.7,y)
# 5=VCC local(-5.08,10.16,270)->abs(x-5.08,y-10.16)
new_nodes.append(label('GND', UX, UY - 10.16))          # pin1 OE-bar -> GND (disabled=never; active low enable tied low = always enabled)
new_nodes.append(label('LED_DATA', UX - 15.24, UY))      # pin2 A
new_nodes.append(label('GND', UX - 5.08, UY + 10.16))    # pin3 GND
new_nodes.append(label('LED_DATA5', UX + 12.7, UY))      # pin4 Y
new_nodes.append(label('VBUS', UX - 5.08, UY - 10.16))   # pin5 VCC

C29X, C29Y = g(380), g(20)
new_nodes.append(build_symbol('Device:C', 'C29', C29X, C29Y, 0,
                               cap_props('C29', '100n', 'Capacitor_SMD:C_0603_1608Metric')))
new_nodes.append(label('VBUS', C29X, C29Y - 3.81))
new_nodes.append(label('GND', C29X, C29Y + 3.81))

# ---------------------------------------------------------------------------
# 3. C21 + C22 bulk caps
# ---------------------------------------------------------------------------

for ref, cx, cy in [('C21', g(360), g(35)), ('C22', g(375), g(35))]:
    new_nodes.append(build_symbol('Device:C', ref, cx, cy, 0,
                                   cap_props(ref, '22u', 'Capacitor_SMD:C_0805_2012Metric')))
    new_nodes.append(label('VBUS', cx, cy - 3.81))
    new_nodes.append(label('GND', cx, cy + 3.81))

print("stage 2/3 nodes so far:", len(new_nodes))

# ---------------------------------------------------------------------------
# 4. S1 8-position DIP switch
# ---------------------------------------------------------------------------

SX, SY = g(360), g(55)
s1_props = [
    ('Reference', 'S1', 0, -12.7, False),
    ('Value', 'DIP switch, 8-position', 0, 12.7, False),
    ('Footprint', 'Button_Switch_THT:SW_DIP_SPSTx08_Slide_9.78x22.5mm_W7.62mm_P2.54mm', 0, 0, True),
    ('Datasheet', '', 0, 0, True),
    ('Description', 'DIP bank for the 8 DIP config bits, internal ESP32 pull-ups, other side to GND', 0, 0, True),
]
new_nodes.append(build_symbol('Switch:SW_DIP_x08', 'S1', SX, SY, 0, s1_props))

DIP_TO_IO = {1: 'IO38', 2: 'IO39', 3: 'IO40', 4: 'IO41', 5: 'IO42', 6: 'IO47', 7: 'IO21', 8: 'IO13'}
for k in range(1, 9):
    y_local = 10.16 - 2.54 * (k - 1)
    left_abs = (SX - 7.62, SY - y_local)
    right_abs = (SX + 7.62, SY - y_local)
    new_nodes.append(label(f'DIP{k}', left_abs[0], left_abs[1]))
    new_nodes.append(label('GND', right_abs[0], right_abs[1]))

# ---------------------------------------------------------------------------
# 5. SW3 test button
# ---------------------------------------------------------------------------

WX, WY = g(360), g(75)
sw3_props = [
    ('Reference', 'SW3', 0, -3.81, False),
    ('Value', 'TS-1002-BR16026', 0, 3.81, False),
    ('Footprint', 'Button_Switch_THT:SW_PUSH_6mm_H13mm', 0, 0, True),
    ('Datasheet', '', 0, 0, True),
    ('Description', '6x6mm THT tact switch, test button', 0, 0, True),
]
new_nodes.append(build_symbol('Switch:SW_Push', 'SW3', WX, WY, 0, sw3_props))
# Switch:SW_Push pin1 local(-5.08,0,0)->abs(x-5.08,y); pin2 local(5.08,0,180)->abs(x+5.08,y)
new_nodes.append(label('BTN_TEST', WX - 5.08, WY))
new_nodes.append(label('GND', WX + 5.08, WY))

# ---------------------------------------------------------------------------
# 6. U1 GPIO re-labeling: replace 8 no_connects with DIP labels, rename BTN_TEST
# ---------------------------------------------------------------------------

U1_PIN_COORDS = {
    'IO38': (219.71, 142.24), 'IO39': (219.71, 144.78), 'IO40': (219.71, 147.32),
    'IO41': (219.71, 149.86), 'IO42': (219.71, 152.4), 'IO47': (219.71, 160.02),
    'IO21': (219.71, 132.08), 'IO13': (189.23, 154.94),
}
IO_TO_DIP = {v: k for k, v in DIP_TO_IO.items()}

removed_nc_coords = set()
for io, (x, y) in U1_PIN_COORDS.items():
    dipnum = IO_TO_DIP[io]
    new_nodes.append(label(f'DIP{dipnum}', x, y))
    removed_nc_coords.add((x, y))

new_top = []
removed_nc_count = 0
renamed_touch_sense = False
for node in root:
    if isinstance(node, list) and node and str(node[0]) == 'no_connect':
        at_node = [c for c in node if isinstance(c, list) and str(c[0]) == 'at'][0]
        p = (float(at_node[1]), float(at_node[2]))
        if any(abs(p[0]-d[0]) < 1e-6 and abs(p[1]-d[1]) < 1e-6 for d in removed_nc_coords):
            removed_nc_count += 1
            continue
        new_top.append(node)
        continue
    if isinstance(node, list) and node and str(node[0]) == 'label':
        at_node = [c for c in node if isinstance(c, list) and str(c[0]) == 'at'][0]
        p = (float(at_node[1]), float(at_node[2]))
        if unquote(node[1]) == 'TOUCH_SENSE' and abs(p[0]-189.23) < 1e-6 and abs(p[1]-157.48) < 1e-6:
            node[1] = quote('BTN_TEST')
            renamed_touch_sense = True
        new_top.append(node)
        continue
    new_top.append(node)

root[:] = new_top
assert removed_nc_count == 8, removed_nc_count
assert renamed_touch_sense

print("Removed 8 no_connects at U1 GPIOs, renamed TOUCH_SENSE->BTN_TEST")

# ---------------------------------------------------------------------------
# 7. D10-D46 LED chain + C30-C66 per-LED decoupling
# ---------------------------------------------------------------------------

X0, Y0 = g(360), g(90)
DX, DY = g(14), g(14)
COLS = 8

for i in range(37):
    d_num = 10 + i
    ref = f'D{d_num}'
    col = i % COLS
    row = i // COLS
    lx = X0 + col * DX
    ly = Y0 + row * DY
    new_nodes.append(build_symbol('LED:SK6812MINI-E', ref, lx, ly, 0, led_props(ref)))

    # DIN pin: local(-7.62,0)->abs(x-7.62,y)
    din_net = 'LED_DATA5' if i == 0 else f'LED_CH{i:02d}'
    new_nodes.append(label(din_net, lx - 7.62, ly))

    # DOUT pin: local(7.62,0)->abs(x+7.62,y)
    if i == 36:
        new_nodes.append(no_connect(lx + 7.62, ly))
    else:
        dout_net = f'LED_CH{i+1:02d}'
        new_nodes.append(label(dout_net, lx + 7.62, ly))

    # VDD pin: local(0,7.62)->abs(x,y-7.62); VSS pin: local(0,-7.62)->abs(x,y+7.62)
    new_nodes.append(label('VBUS', lx, ly - 7.62))
    new_nodes.append(label('GND', lx, ly + 7.62))

    # per-LED cap, offset below the LED within the same grid cell
    cap_ref = f'C{30 + i}'
    cx, cy = lx, ly + DY / 2
    new_nodes.append(build_symbol('Device:C', cap_ref, cx, cy, 0,
                                   cap_props(cap_ref, '100n', 'Capacitor_SMD:C_0603_1608Metric')))
    new_nodes.append(label('VBUS', cx, cy - 3.81))
    new_nodes.append(label('GND', cx, cy + 3.81))

print("total new_nodes:", len(new_nodes))

sheet_instances_idx = None
for idx, node in enumerate(root):
    if isinstance(node, list) and node and str(node[0]) == 'sheet_instances':
        sheet_instances_idx = idx
        break
assert sheet_instances_idx is not None
root[sheet_instances_idx:sheet_instances_idx] = new_nodes

out = serialize(tree[0]) + '\n'
open(SCH, 'w').write(out)
print("Wrote", SCH)



