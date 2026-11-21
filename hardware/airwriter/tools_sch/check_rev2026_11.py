#!/usr/bin/env python3
"""Rev 2026.11 netlist gate: every net the design changes, pin for pin.

W-1176: each socket has the same block, so either can be the printer port or
LocalTalk: a THVD1452 (U2 for J2, U6 for J4) with ~RE low, its own TashTalk
(U10, U7), RO to the PIC's data net through 2.2 k, and a GPIO through 220 R to
the data net and to DE.

    kicad-cli sch export netlist --format kicadxml -o net.xml airwriter.kicad_sch
    python3 tools_sch/check_rev2026_11.py net.xml
"""
import sys
import xml.etree.ElementTree as ET

WANT = {
    '/BOOT_CFG': {'U1.27', 'SW2.1', 'R21.2'},
    '/LT_DE': {'U6.3', 'U7.3', 'R22.1', 'R44.2'},
    '/LED_DATA': {'U1.25', 'U9.2', 'R23.1'},
    # U3's driver inputs and receiver 1's output on GPIOs the DIP switches freed,
    # so IO35 and IO37 stay free for an octal-PSRAM module
    '/HSKI_DRV': {'U1.24', 'U3.10', 'R24.1'},            # IO47: J2's HSKo
    '/J4_HSKO_DRV': {'U1.21', 'U3.11'},                  # IO13: J4's HSKo, firmware-held
    '/UART_TX': {'U1.10', 'R38.1'},                      # IO17: J2's data, through R38
    '/LT_RESET': {'U1.38', 'R27.1'},
    '/LT_MCLR': {'R27.2', 'U7.4'},
    '/PR_TXD_P': {'J2.8', 'R29.1'},
    '/PR_TXD_P_IN': {'R29.2', 'U2.9'},
    '/PR_TXD_N': {'J2.5', 'R30.1'},
    '/PR_TXD_N_IN': {'R30.2', 'U2.8'},
    # U3's channels: J2 on driver 2 / receiver 2 (pins 7-10), J4 on driver 1 / receiver 1 (11-14)
    '/J4_HSKO': {'U3.14', 'J4.1'},
    '/J4_HSKI': {'U3.13', 'J4.2'},
    '/J4_HSKI_RX': {'U3.12', 'U1.23'},                   # IO21
    **{f'/SPARE_IO{n}': {f'U1.{n - 7}'} for n in (38, 39, 40)},   # to bare pads on the board
    '/BOARD_REV': {'U1.9', 'R32.1'},
    '/BOARD_LC': {'U1.8', 'R33.1'},
    '/HSKI_RS232': {'U3.7', 'R9.1'},
    '/PR_HSKO': {'U3.8', 'J2.2'},
    '/UART_CTS': {'U3.9', 'U1.12'},
    '/PR_HSKI': {'J2.1', 'R9.2'},
    '/LT_RXD_N': {'R10.1', 'U6.8'},
    '/LT_RXD_P': {'R11.1', 'U6.9'},
    '/LT_TXD_N': {'R12.1', 'U6.7', 'R46.1'},
    '/LT_TXD_P': {'R13.1', 'U6.6', 'R45.2'},
    # W-1176, J2's block
    '/UART_RX': {'U1.11', 'U2.1', 'R37.1'},              # IO18: J2's receiver
    '/LT2_DATA': {'U2.4', 'U10.2', 'R37.2', 'R38.2'},
    '/LT2_DE': {'U2.3', 'U10.3', 'R36.1', 'R39.2'},
    '/J2_DE': {'U1.7', 'R39.1'},                          # IO7
    '/PR_RXD_P': {'U2.6', 'J2.6', 'R40.2'},
    '/PR_RXD_N': {'U2.7', 'J2.3', 'R41.1'},
    '/LT2_MCLR': {'U1.35', 'U10.4'},                      # IO42
    '/LT2_CTS': {'U1.34', 'U10.5'},                       # IO41
    '/LT2_ICSPCLK': {'U10.6', 'R34.2'},
    '/LT2_ICSPDAT': {'U10.7', 'R35.2'},
    '/LT2_UART_TX': {'U1.19', 'R34.1'},                   # IO11
    '/LT2_UART_RX': {'U1.20', 'R35.1'},                   # IO12
    # W-1176, J4's block
    '/J4_RX': {'U1.18', 'U6.1', 'R42.1'},                 # IO10
    '/LT_DATA': {'U6.4', 'U7.2', 'R42.2', 'R43.2'},
    '/J4_TX': {'U1.17', 'R43.1'},                         # IO9
    '/J4_DE': {'U1.15', 'R44.1'},                         # IO3
    '/LT_PORT_RXD_N': {'R10.2', 'J4.5'},
    '/LT_PORT_RXD_P': {'R11.2', 'J4.8'},
    '/LT_PORT_TXD_N': {'R12.2', 'J4.3'},
    '/LT_PORT_TXD_P': {'R13.2', 'J4.6'},
}
ON_RAIL = {'/+3V3': {'R21.1', 'U2.10', 'U6.10', 'U10.1', 'C68.1', 'R40.1', 'R45.1'},
           '/GND': {'R22.2', 'R23.2', 'R24.2', 'R32.2', 'R33.2', 'R36.2', 'R41.2', 'R46.2',
                    'U2.2', 'U2.5', 'U6.2', 'U6.5', 'U10.8', 'C68.2'}}   # U2.2, U6.2: ~RE, receivers always on
OFF_RAIL = {'/GND': {'U3.8', 'U3.10'}}
GONE_REFS = {'C7', 'R5', 'R6', 'R7', 'R26', 'R28', 'R31', 'S1',  # R26: IO3 boxed in; S1, R28: no DIPs; R31: see the design
             'U8', 'R14', 'R25', 'C18'}                          # W-1176: one THVD1452 per socket
FREE = {'U1.28', 'U1.29', 'U1.30'}     # IO35-37: an octal-PSRAM module's own
GONE_NETS = {*(f'/DIP{n}' for n in range(1, 9)), '/DIP3_GPIO', '/LT_A', '/LT_B', '/LT_Y', '/LT_Z', '/LT_PORT_A', '/LT_PORT_B', '/LT_PORT_Y', '/LT_PORT_Z'}
PARTS = {  # ref: (value, LCSC)
    'U1': ('ESP32-S3-WROOM-1-N16R8', 'C2913202'),       # its octal PSRAM takes IO35-IO37 (FREE)
    'U2': ('THVD1452DGSR', 'C2879202'), 'U6': ('THVD1452DGSR', 'C2879202'),   # DE and ~RE, +-15 V inputs, +-30 kV
    'U10': ('PIC12F1840-I/SN', 'C39594'),                # J2's TashTalk, flashed in place
    'U7': ('PIC12F1840-I/SN', 'C39594'),                 # J4's, soldered too: no socket (W-1176)
    'R10': ('15k', 'C22809'), 'R11': ('15k', 'C22809'),  # J4's receive pair, as J2's R29/R30
    **{f'R{n}': ('2.2k', 'C4190') for n in (37, 42)},
    **{f'R{n}': ('220R', 'C22962') for n in (38, 39, 43, 44)},
    **{f'R{n}': ('4.7k', 'C23162') for n in (40, 41, 45, 46)},
    **{f'R{n}': ('100R', 'C22775') for n in (34, 35)},
    'U3': ('SP3232EBEN-L/TR', 'C6852'), 'U9': ('SN74LV1T125DCKR', 'C75925'),
    'R17': ('150R', 'C22808'), 'SW3': ('TS-1002-BR15026', 'C920209'), 'D3': (None, 'C20613324'),
    'R9': ('0R', 'C21189'), 'R27': ('1k', 'C21190'), 'SW1': ('TSB001A3518A', 'C2888448'),
    'R29': ('15k', 'C22809'), 'R30': ('15k', 'C22809'),
    **{f'R{n}': ('10k', 'C25804') for n in (21, 22, 23, 24, 32, 33, 36)},
}

root = ET.parse(sys.argv[1]).getroot()
nets = {n.get('name'): {f"{x.get('ref')}.{x.get('pin')}" for x in n.iter('node')} for n in root.iter('net')}
comps = {c.get('ref'): c for c in root.iter('comp')}
bad = []
for net, pins in WANT.items():
    if nets.get(net) != pins:
        bad.append(f'{net}: want {sorted(pins)} got {sorted(nets.get(net, set()))}')
for net, pins in ON_RAIL.items():
    if not pins <= nets.get(net, set()):
        bad.append(f'{net} missing {sorted(pins - nets.get(net, set()))}')
for net, pins in OFF_RAIL.items():
    if pins & nets.get(net, set()):
        bad.append(f'{net} still has {sorted(pins & nets[net])}')
bad += [f'{r} still in the schematic' for r in GONE_REFS if r in comps]
bad += [f'{n} still a net' for n in GONE_NETS if n in nets]
bad += [f'{pin} is on {n}' for n, pins in nets.items() if not n.startswith('unconnected')
        for pin in pins & FREE]
for ref, (val, lcsc) in PARTS.items():
    c = comps.get(ref)
    if c is None:
        bad.append(f'{ref} missing'); continue
    fields = {f.get('name'): f.text for f in c.iter('field')}
    if val and c.findtext('value') != val:
        bad.append(f'{ref} value {c.findtext("value")} != {val}')
    if fields.get('LCSC') != lcsc:
        bad.append(f'{ref} LCSC {fields.get("LCSC")} != {lcsc}')
    if any(p.get('name') == 'dnp' for p in c.iter('property')):
        bad.append(f'{ref} is DNP')
if comps['D3'].findtext('footprint') != 'airwriter:LED_D3.0mm_Spacer_H8mm':
    bad.append(f"D3 footprint {comps['D3'].findtext('footprint')}")
print('\n'.join(bad) if bad else f'ok: {len(WANT)} nets, {len(PARTS)} parts, {len(GONE_REFS)} removed')
sys.exit(1 if bad else 0)
