#!/usr/bin/env python3
"""Rev 2026.11: the rev A copper and parts that move to make room.

    <KiCad python> tools_pcb/r13_reroute.py

Run by r13_build.sh after r13_clean.py and r13_place.py, with KiCad closed,
once per build (it refuses a board it has already changed).

U3 swaps channels: the printer port takes driver 2 and receiver 2 (pins 7-10)
and the LocalTalk port driver 1 and receiver 1 (11-14), so J4's two RS-232
lines leave U3 on the side facing J4 and run straight down beside it, under
U8, to J4. IO35 drives both drivers' inputs (pins 10 and 11): the cable test
still sees each handshake wire, and no GPIO is left that reaches U3. The
printer port's lines come back to rev A's routes by short steps.

To clear that path:
- U3's 3.3 V feed runs under U3's body from pin 16, not down its right side.
- C18 moves next to U8's VCC pin and R10/R11 west of it; R14 moves to LT_DATA
  between U7 and U6. The LocalTalk receive lines follow them.
- R27 joins LT_RESET where it already passes east of U7; U7.4 is fed back
  along rev A's run.
- SW2 and R21 move west so R28 sits right under IO40; DIP1 meets its pin from
  below instead of looping round under the module.
- LED_DATA's run north of the module lifts 0.37 mm so R32 and R33 get ground
  vias.

And two rev A fixes from the rev B list: the VBUS via at the H4 crossing grows
from a 0.4 mm to a 0.5 mm drill (a partner via has no room between C22 and
H4's screw head), and D- reaches the ESP32 through U5's pad 3, as D+ goes through U5, so
U5 no longer hangs off D- on an 8 mm stub; U5's ground pin, cut off from the
pour by the new D- run, gets a via under U5.
"""
import os

import pcbnew

from r13_edit import Board, B, nm

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, '..', 'airwriter.kicad_pcb')


def trunk(e):
    """U3's 3.3 V: pin 16 under the body to the branch at y 144.24."""
    e.drop(e.track('/+3V3', (121.455, 143.055), (121.455, 133.516)),
           e.track('/+3V3', (121.648, 143.248), (121.455, 143.055)),
           e.track('/+3V3', (121.648, 143.829), (121.648, 143.248)),
           e.track('/+3V3', (121.237, 144.24), (121.648, 143.829)),
           e.track('/+3V3', (121.455, 133.516), (121.125, 133.186)))
    e.draw('/+3V3', [(120.05, 132.62), (118.8, 132.62), (117.3, 134.12), (117.3, 144.24)], width=0.5)


def printer_handshakes(e):
    """J2's handshakes on U3's pins 7-10, back onto rev A's routes."""
    # pin 7 (T2OUT) to R9: a step up beside the pin to rev A's run between pins 5 and 6
    e.draw('/HSKI_RS232', [(115.1, 140.24), (116.508, 140.24), (116.508, 138.335)])
    # pin 8 (R2IN) from J2.2: straight down to rev A's run
    e.drop(e.track('/PR_HSKO', (117.641, 138.508), (117.641, 142.52)),
           e.track('/PR_HSKO', (117.641, 142.52), (114.65, 145.512)))
    e.draw('/PR_HSKO', [(115.1, 141.51), (115.1, 145.061), (114.65, 145.512)])
    # pin 9 (R2OUT) to IO8: a via on rev A's diagonal, just past the pin
    e.drop(e.via('/UART_CTS', (118.58, 138.494)))
    e.move_end(e.track('/UART_CTS', (122.83, 142.744), (118.58, 138.494)), (118.58, 138.494), (121.6, 141.514))
    e.add_via('/UART_CTS', (121.6, 141.514))
    e.draw('/UART_CTS', [(120.05, 141.51), (121.6, 141.514)])
    # pins 10 and 11 from IO35: joined under U3, a via below it onto rev A's back run,
    # and R24 (the pull-down) below U3
    e.drop(e.track('/HSKI_DRV', (120.05, 138.97), (119.752, 138.97)),
           e.track('/HSKI_DRV', (119.752, 138.97), (118.822, 139.899)),
           e.track('/HSKI_DRV', (118.822, 139.899), (118.822, 141.899)),
           e.track('/HSKI_DRV', (118.822, 141.899), (120.463, 143.539)),
           e.track('/HSKI_DRV', (120.463, 143.539), (120.946, 143.539)),
           e.via('/HSKI_DRV', (120.946, 143.539)))
    e.move_end(e.track('/HSKI_DRV', (120.946, 143.539), (122.116, 144.708)), (120.946, 143.539), (119.75, 142.34))
    e.draw('/HSKI_DRV', [(120.05, 138.97), (118.6, 138.97), (118.6, 142.0), (118.94, 142.34), (119.75, 142.34)])
    e.draw('/HSKI_DRV', [(120.05, 140.24), (118.6, 140.24)])
    e.add_via('/HSKI_DRV', (119.75, 142.34))
    e.draw('/HSKI_DRV', [(119.75, 142.34), (120.2, 142.79)])


def under_u8(e):
    """The row under U8: C18 beside U8's VCC pin, R10/R11 west of it, R14 out."""
    e.drop(e.track('/+3V3', (122.351, 150.105), (122.68, 150.105)),
           e.track('/+3V3', (122.68, 150.105), (124.2, 151.625)),
           e.track('/+3V3', (125.7, 153.12), (124.2, 151.625)),
           e.track('/+3V3', (125.7, 153.225), (125.7, 153.12)),
           e.track('/LT_DATA', (125.7, 151.57), (125.7, 150.28)),
           e.track('/LT_DATA', (125.7, 150.28), (125.88, 150.1)),
           e.track('/LT_RXD_N', (120.815, 151.575), (119.683, 150.443)),
           e.track('/LT_RXD_N', (121.2, 151.575), (120.815, 151.575)),
           e.track('/LT_RXD_P', (119.263, 150.753), (120.813, 152.303)),
           e.track('/LT_RXD_P', (120.813, 152.303), (121.972, 152.303)),
           e.track('/LT_RXD_P', (121.972, 152.303), (122.7, 151.575)),
           e.track('/LT_PORT_RXD_N', (126.495, 158.52), (121.2, 153.225)),
           e.track('/LT_PORT_RXD_P', (124.488, 154.412), (123.887, 154.412)),
           e.track('/LT_PORT_RXD_P', (123.887, 154.412), (122.7, 153.225)),
           e.via('/LT_PORT_RXD_P', (124.488, 154.412)),
           e.track('/LT_PORT_RXD_P', (126.096, 156.02), (124.488, 154.412)),
           e.track('/LT_PORT_RXD_P', (129.1, 156.02), (126.096, 156.02)))
    e.move_end(e.track('/LT_PORT_RXD_N', (129.1, 158.52), (126.495, 158.52)), (126.495, 158.52), (125.295, 158.52))
    e.place('R11', 118.45, 152.4, -90)
    e.place('R10', 120.0, 152.4, -90)
    e.place('C18', 121.55, 152.4, -90)
    e.place('R14', 131.3, 144.6, 180)        # below U7's TASHTALK label
    e.draw('/+3V3', [(120.925, 150.105), (121.55, 150.73), (121.55, 151.62)], width=0.5)
    e.draw('/LT_RXD_N', [(119.683, 150.443), (119.683, 150.9), (120.0, 151.217)])
    e.draw('/LT_RXD_P', [(119.263, 150.753), (118.45, 151.566)])
    e.draw('/LT_PORT_RXD_N', [(120.0, 153.225), (125.295, 158.52)])
    e.draw('/LT_PORT_RXD_P', [(118.45, 153.225), (118.9, 153.675), (118.9, 154.25)])
    e.add_via('/LT_PORT_RXD_P', (118.9, 154.25))
    e.draw('/LT_PORT_RXD_P', [(118.9, 154.25), (122.67, 154.25), (124.44, 156.02), (129.1, 156.02)], B)
    # R14 pulls LT_DATA up from where it passes between U7 and U6
    e.draw('/+3V3', [(132.125, 144.6), (132.927, 144.6)], width=0.3)
    e.draw('/LT_DATA', [(130.23, 144.6), (130.475, 144.6)])


def reset_line(e):
    """R27 (r13_place puts it east of U7) in LT_RESET's back run; U7.4 fed back along it."""
    e.move_end(e.track('/LT_RESET', (125.171, 134.171), (150.717, 134.171)), (125.171, 134.171), (134.12, 134.171))
    e.add_via('/LT_RESET', (134.12, 134.171))
    e.draw('/LT_RESET', [(134.12, 134.171), (134.12, 135.175)])
    e.draw('/LT_MCLR', [(134.12, 136.825), (134.12, 137.9)])
    e.add_via('/LT_MCLR', (134.12, 137.9))
    e.draw('/LT_MCLR', [(134.12, 137.9), (130.39, 134.17), (125.171, 134.171), (124.09, 133.09)], B)


def boot_and_dips(e):
    """SW2 and R21 west, R28 under IO40, DIP1 up from below, DIP3 down to R28."""
    e.drop(e.track('/DIP1', (143.08, 146.911), (143.08, 148.544)),
           e.track('/DIP1', (142.784, 146.614), (143.08, 146.911)),
           e.via('/DIP1', (142.784, 146.614)),
           e.track('/DIP1', (145.938, 148.436), (143.974, 150.4)),
           e.track('/DIP1', (142.923, 146.475), (145.602, 146.475)),
           e.track('/DIP1', (145.938, 146.811), (145.938, 148.436)),
           e.track('/DIP1', (142.784, 146.614), (142.923, 146.475)),
           e.track('/DIP1', (145.602, 146.475), (145.938, 146.811)))
    e.move_end(e.track('/DIP1', (143.974, 150.4), (113.405, 150.4)), (143.974, 150.4), (143.08, 150.4))
    e.draw('/DIP1', [(143.08, 150.4), (143.08, 150.95)], B)
    e.add_via('/DIP1', (143.08, 150.95))
    e.draw('/DIP1', [(143.08, 150.95), (143.08, 148.6)])
    e.drop(e.track('/DIP3', (145.352, 147.275), (145.352, 147.041)),
           e.via('/DIP3', (145.352, 147.041)),
           e.track('/DIP3', (145.227, 147.166), (145.352, 147.041)))
    e.move_end(e.track('/DIP3', (141.113, 147.166), (145.227, 147.166)), (145.227, 147.166), (144.6, 147.166))
    e.draw('/DIP3', [(144.6, 147.166), (144.6, 151.925)], B)
    e.add_via('/DIP3', (144.6, 151.925))
    e.draw('/DIP3', [(144.6, 151.925), (145.62, 151.925)])
    e.draw('/DIP3_GPIO', [(145.62, 150.275), (145.62, 148.6)])
    e.drop(e.track('/BOOT_CFG', (140.421, 149.546), (141.3, 150.425)),
           e.track('/BOOT_CFG', (142.8, 151.2), (142.075, 151.2)),
           e.track('/BOOT_CFG', (142.075, 151.2), (141.3, 150.425)),
           e.track('/BOOT_CFG', (138.0, 149.546), (140.421, 149.546)))
    e.place('SW2', 140.0, 152.7, 0)
    boot = [t for t in e.b.FindFootprintByReference('SW2').GraphicalItems()
            if isinstance(t, pcbnew.PCB_TEXT) and t.GetText() == 'BOOT'][0]
    boot.SetPosition(pcbnew.VECTOR2I(nm(143.1), nm(152.7)))   # J4's outline is below
    boot.SetTextAngleDegrees(90)
    e.place('R21', 140.0, 150.2, 180)
    e.draw('/BOOT_CFG', [(138.0, 149.546), (138.65, 150.2), (139.175, 150.2)])
    e.draw('/BOOT_CFG', [(138.0, 149.546), (138.0, 151.5), (138.3, 151.8)])


def led_data(e):
    """LED_DATA's run north of the module, up 0.37 mm."""
    e.drop(e.track('/LED_DATA', (144.75, 127.967), (139.494, 127.967)),
           e.track('/LED_DATA', (139.494, 127.967), (137.298, 130.163)))
    e.move_end(e.track('/LED_DATA', (146.7, 126.017), (144.75, 127.967)), (144.75, 127.967), (145.117, 127.6))
    e.draw('/LED_DATA', [(137.298, 130.163), (139.861, 127.6), (145.117, 127.6)])


def usb(e):
    """VBUS's one via, bigger; D- in line through U5."""
    e.drop(e.via('/VBUS', (151.25, 150.171)))
    e.move_end(e.track('/VBUS', (151.338, 150.386), (151.25, 150.171)), (151.25, 150.171), (151.3, 150.1))
    e.move_end(e.track('/VBUS', (151.25, 150.171), (154.538, 146.883)), (151.25, 150.171), (151.3, 150.1))
    e.add_via('/VBUS', (151.3, 150.1), size=0.9, drill=0.5)
    e.drop(e.track('/USB_DM', (146.691, 157.346), (146.691, 155.674)),
           e.track('/USB_DM', (146.691, 155.674), (148.323, 154.042)))
    e.move_end(e.track('/USB_DM', (146.691, 159.673), (146.691, 157.346)), (146.691, 157.346), (146.691, 157.3))
    e.draw('/USB_DM', [(146.691, 157.3), (143.15, 157.3)])
    e.add_via('/GND', (144.1, 159.35))
    e.draw('/GND', [(144.1, 159.35), (144.1, 158.26)])


def main():
    e = Board(BOARD)
    if any(t.GetNetname() == '/LT_MCLR' and t.GetLayer() == B for t in e.b.GetTracks()):
        print('reroute: already done')
        return
    trunk(e)
    printer_handshakes(e)
    under_u8(e)
    reset_line(e)
    boot_and_dips(e)
    led_data(e)
    usb(e)
    e.save()
    print(f'reroute: {len(e.dead)} rev A pieces replaced; R10, R11, C18, R14, SW2, R21 moved')


if __name__ == '__main__':
    main()
