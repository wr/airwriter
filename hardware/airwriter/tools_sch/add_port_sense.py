#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""W-1185: add each socket's port sense to rev 2026.11's schematic.

    python3 tools_sch/add_port_sense.py            # from hardware/airwriter
    kicad-cli sch upgrade --force airwriter.kicad_sch
    python3 tools_sch/keep_order.py old.kicad_sch airwriter.kicad_sch out.kicad_sch

Per socket, a dual NPN and five resistors (docs/superpowers/specs/
2026-11-23-port-sense-design.md): Qa turns on when the socket's pin 8 is held
above about 2.5 V, Qb when its pin 5 is held below about -3 V, and their joined
collectors pull the socket's sense line low through 4.7 k. J2 (Port 1) gets Q1
and R47-R51 on IO39, J4 (Port 2) Q2 and R52-R56 on IO38 (so the two lines don't
cross on the board), whose labels become J2_SENSE and J4_SENSE. Everything joins by net labels on the pins, as the rest
of the sheet does, in an empty stretch east of the sockets. A second run finds
Q1 and stops.
"""
import copy
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, serialize, unquote, quote, Sym  # noqa: E402

SCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'airwriter.kicad_sch')
PROJECT_PATH = '/b907fa76-c5fe-4a7a-8e4e-adfdab62cb9c'
KICAD_SYM = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/Transistor_BJT.kicad_sym'
BASE = 'Q_Dual_NPN_NPN_E1B1C2E2B2C1'      # BC847BDW1's pins: 1 E1, 2 B1, 6 C1; 4 E2, 5 B2, 3 C2

R0402 = 'Resistor_SMD:R_0402_1005Metric'
RES = {   # value: LCSC, filled from the product pages
    '22k': 'C25768', '6.8k': 'C25917', '15k': 'C25756', '4.7k': 'C25900',
}
Q_LCSC = 'C82368'                          # onsemi BC847BDW1T1G

SOCKETS = (
    # socket, Q, (R 22k, R 6.8k, R 15k, R 6.8k, R 4.7k), pin 8 net, pin 5 net, GPIO label, origin
    ('J2', 'Q1', ('R47', 'R48', 'R49', 'R50', 'R51'), 'PR_TXD_P', 'PR_TXD_N', 'J2_SENSE', 'SPARE_IO39', (469.9, 220.98)),
    ('J4', 'Q2', ('R52', 'R53', 'R54', 'R55', 'R56'), 'LT_PORT_RXD_P', 'LT_PORT_RXD_N', 'J4_SENSE', 'SPARE_IO38', (469.9, 256.54)),
)


def S(name):
    return Sym(name)


def L(*items):
    return list(items)


def new_uuid():
    return quote(str(uuid.uuid4()))


def num(v):
    return Sym(('%.2f' % v).rstrip('0').rstrip('.'))


def at(x, y, rot=0):
    return L(S('at'), num(x), num(y), num(rot))


def effects(hide=False):
    e = [S('effects'), L(S('font'), L(S('size'), Sym('1.27'), Sym('1.27')))]
    return e


def prop(name, value, x, y, hide=False):
    node = [S('property'), quote(name), quote(value), at(x, y)]
    if hide:
        node.append(L(S('hide'), S('yes')))
    node.append(effects())
    return node


def label(text, x, y, rot=0):
    return L(S('label'), quote(text), at(x, y, rot),
             L(S('effects'), L(S('font'), L(S('size'), Sym('1.27'), Sym('1.27'))),
               L(S('justify'), S('left' if rot == 0 else 'right'), S('bottom'))),
             L(S('uuid'), new_uuid()))


def symbol(lib_id, ref, unit, x, y, props, pins):
    node = [S('symbol'), L(S('lib_id'), quote(lib_id)), at(x, y), L(S('unit'), Sym(str(unit))),
            L(S('exclude_from_sim'), S('no')), L(S('in_bom'), S('yes')), L(S('on_board'), S('yes')),
            L(S('dnp'), S('no')), L(S('uuid'), new_uuid())]
    for name, value, dx, dy, hide in props:
        node.append(prop(name, value, x + dx, y + dy, hide))
    for p in pins:
        node.append(L(S('pin'), quote(p), L(S('uuid'), new_uuid())))
    node.append(L(S('instances'), L(S('project'), quote('airwriter'),
                  L(S('path'), quote(PROJECT_PATH), L(S('reference'), quote(ref)), L(S('unit'), Sym(str(unit)))))))
    return node


def resistor(ref, value, x, y, top_net, bottom_net, why):
    """Device:R upright: pin 1 at (x, y - 3.81), pin 2 at (x, y + 3.81)."""
    props = [('Reference', ref, 2.54, -1.27, False), ('Value', value, 2.54, 1.27, False),
             ('Footprint', R0402, 0, 0, True), ('Datasheet', '', 0, 0, True),
             ('Description', 'RES %s 1%% 1/16W 0402, %s' % (value, why), 0, 0, True),
             ('LCSC', RES[value], 0, 0, True), ('MPN', '', 0, 0, True), ('Manufacturer', 'Any', 0, 0, True)]
    return [symbol('Device:R', ref, 1, x, y, props, ['1', '2']),
            label(top_net, x, y - 3.81), label(bottom_net, x, y + 3.81)]


def transistor(ref, unit, x, y, b, c, e, why):
    """BC847BDW1 unit: B at (x - 5.08, y), C at (x + 2.54, y - 5.08), E at (x + 2.54, y + 5.08)."""
    props = [('Reference', ref, 5.08, -1.27, False), ('Value', 'BC847BDW1', 5.08, 1.27, False),
             ('Footprint', 'Package_TO_SOT_SMD:SOT-363_SC-70-6', 0, 0, True),
             ('Datasheet', 'http://www.onsemi.com/pub_link/Collateral/BC846BDW1T1-D.PDF', 0, 0, True),
             ('Description', 'Dual NPN 45 V 100 mA, SOT-363, VEBO 6 V; %s' % why, 0, 0, True),
             ('LCSC', Q_LCSC, 0, 0, True), ('MPN', 'BC847BDW1T1G', 0, 0, True),
             ('Manufacturer', 'onsemi', 0, 0, True)]
    pins = ['2', '6', '1'] if unit == 1 else ['5', '3', '4']
    return [symbol('Transistor_BJT:BC847BDW1', ref, unit, x, y, props, pins),
            label(b, x - 5.08, y, 180), label(c, x + 2.54, y - 5.08), label(e, x + 2.54, y + 5.08)]


def lib_symbol():
    """KiCad's BC847BDW1 flattened as KiCad caches it: the base symbol's units and
    pins, under the derived symbol's name and with its fields."""
    lib = parse(open(KICAD_SYM).read())[0]
    find = lambda name: next(copy.deepcopy(s) for s in lib[1:]
                             if isinstance(s, list) and str(s[0]) == 'symbol' and unquote(s[1]) == name)
    src, derived = find(BASE), find('BC847BDW1')

    def rename(node):
        for i, c in enumerate(node):
            if isinstance(c, str) and c.startswith('"') and unquote(c).startswith(BASE + '_'):
                node[i] = quote('BC847BDW1' + unquote(c)[len(BASE):])
            elif isinstance(c, list):
                rename(c)
    rename(src)
    src[1] = quote('Transistor_BJT:BC847BDW1')
    fields = {unquote(c[1]): c for c in derived if isinstance(c, list) and str(c[0]) == 'property'}
    for i, c in enumerate(src):
        if isinstance(c, list) and str(c[0]) == 'property' and unquote(c[1]) in fields:
            src[i] = fields.pop(unquote(c[1]))
    last = max(i for i, c in enumerate(src) if isinstance(c, list) and str(c[0]) == 'property')
    src[last + 1:last + 1] = list(fields.values())
    return src


def main():
    text = open(SCH).read()
    if '"Q1"' in text:
        raise SystemExit('Q1 is already in the schematic')
    tree = parse(text)
    root = tree[0]
    lib_symbols = next(n for n in root if isinstance(n, list) and str(n[0]) == 'lib_symbols')
    lib_symbols.append(lib_symbol())

    new = []
    for sock, q, (r22, r68a, r15, r68b, r47), p8, p5, sense, spare, (x0, y0) in SOCKETS:
        sp, sn, sns = sock + '_SNS_P', sock + '_SNS_N', sock + '_SNS'
        new += resistor(r22, '22k', x0, y0, p8, sp, '%s pin 8 to Qa\'s base' % sock)
        new += resistor(r68a, '6.8k', x0 + 7.62, y0, sp, 'GND', 'Qa\'s base to GND')
        new += transistor(q, 1, x0 + 20.32, y0, sp, sns, 'GND', 'Qa: %s pin 8 held high' % sock)
        new += resistor(r15, '15k', x0, y0 + 16.51, p5, sn, '%s pin 5 to Qb\'s emitter' % sock)
        new += resistor(r68b, '6.8k', x0 + 7.62, y0 + 16.51, sn, 'GND', 'Qb\'s emitter to GND')
        new += transistor(q, 2, x0 + 20.32, y0 + 16.51, 'GND', sns, sn, 'Qb: %s pin 5 held low' % sock)
        new += resistor(r47, '4.7k', x0 + 33.02, y0 + 7.62, sns, sense,
                        'keeps the sense line above -0.3 V when Qb pulls alone')
        renamed = 0
        for n in root:
            if isinstance(n, list) and str(n[0]) == 'label' and unquote(n[1]) == spare:
                n[1] = quote(sense)
                renamed += 1
        if renamed != 1:
            raise SystemExit('%d %s labels, not 1' % (renamed, spare))

    idx = next(i for i, n in enumerate(root) if isinstance(n, list) and str(n[0]) == 'sheet_instances')
    root[idx:idx] = new
    open(SCH, 'w').write(serialize(root) + '\n')
    print('added Q1, Q2, R47-R56 and renamed IO39 and IO38 to J2_SENSE and J4_SENSE')


if __name__ == '__main__':
    main()
