# Licensing

Copyright © 2026 Wells Riley.

AirWriter is open hardware and open-source firmware. Each part of the repository carries the
licence that fits it; the full texts are in [`LICENSES/`](LICENSES/).

| Path | What it is | Licence |
|---|---|---|
| `firmware/`, `tools/`, `fixtures/` | ESP32 firmware, host tools and their test data | [GPL-3.0-or-later](LICENSES/GPL-3.0-or-later.txt) |
| `hardware/`, `enclosure/` | KiCad board and panel, case CAD and its printable outputs | [CERN-OHL-S-2.0](LICENSES/CERN-OHL-S-2.0.txt) |
| `docs/`, `AGENTS.md`, `CLAUDE.md`, the wiki | Documentation | [CC-BY-SA-4.0](LICENSES/CC-BY-SA-4.0.txt) |

Source files carry an SPDX header where the format allows one.

**Source location** (CERN-OHL-S §4): https://github.com/wr/airwriter

## Name and logo

The licences above cover the design, not the brand. "AirWriter" and the AirWriter logo are not
licensed under any of them. You may build, modify and sell hardware from this design under
CERN-OHL-S, but please give it a different name and don't use the logo, so nobody mistakes it for
the original. The same goes for the Wells Workshop name and mark, which the settings page shows in
its footer (`tools/webui/wells-workshop.svg`), and for the render of AirWriter the page uses as its
mark (`tools/webui/airwriter.webp`).

## Third-party material

| Item | Where | Licence / terms |
|---|---|---|
| TashTalk 2.1.3 firmware (lampmerchant) | `firmware/components/pic_prog/tashtalk-2.1.3.hex`, source alongside | GPL-3.0; see that folder's `NOTICE` |
| URF line decoding, ported from `_cupsRasterReadPixels()` in OpenPrinting CUPS `cups/raster-stream.c` | `firmware/components/urf_decode/urf_decode.c` | Apache-2.0 (CUPS); compatible with GPL-3.0 |
| WS2812B-2020 3D model, KiCad libraries | `hardware/airwriter/lib/airwriter.3dshapes/LED_WS2812B-2020_PLCC4_2.0x2.0mm.step` | CC-BY-SA-4.0 with the KiCad libraries exception |
| Kycon KMDGX-8S-BS 3D model, from Kycon | `hardware/airwriter/lib/airwriter.3dshapes/MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle.step` | Manufacturer's model, redistributed for design use |
| Apple's colour ImageWriter icon, redrawn pixel for pixel, used as the printer's icon and the settings page's favicon and sidebar | `tools/icon/imagewriter.txt`, `firmware/components/ipp_server/icons/icon-*.png` (the `laser-*.png` LaserWriter beside them is drawn for AirWriter, under the firmware's licence), inlined in `firmware/components/webui/webui_page.h`, and as the test page's letterhead in `firmware/components/testpage/tp_art.h` | © Apple; not licensed under this repository's licences |
| Apple's Color StyleWriter tone curves and 16 x 16 halftone screen, and its printer icon (the Chooser icon, in Apple's colours) | `firmware/components/halftone/include/apple_tables.h`, `firmware/components/halftone/apple_tables.c`, `tools/icon/stylewriter.txt`, `firmware/components/ipp_server/icons/stylewriter-*.png`, and as the StyleWriter test page's letterhead in `firmware/components/stylewriter/sw_art.h` | © Apple; extracted from Apple's Color StyleWriter 1500, 2200 and 2500 drivers by `tools/stylewriter/mktables.py`; not licensed under this repository's licences |
| The scanline coder of lpstyl 0.9.9 (Monroe Williams, https://github.com/Godzil/lpstyl), ported for the host tests only | `tools/rip/test/lpstyl_encode.c`, `tools/rip/test/lpstyl_encode.h`; not in the firmware | BSD-2-Clause, © 1996-2000 Monroe Williams; the notice is kept in the file |
| Apple Garamond Light, subset to printable ASCII and a few marks, for the settings page's titles | inlined in `tools/webui/style.css` and `firmware/components/webui/webui_page.h`; "AirWriter" and "Test page" set in it, as dots, in `firmware/components/testpage/tp_art.h` and `firmware/components/stylewriter/sw_art.h` | © Apple; not licensed under this repository's licences |
| Basilisk II's application icon (macemu, `BasiliskII/src/MacOSX/BasiliskII.icns`), scaled to 72 px | `tools/webui/basilisk-ii.png`, inlined in `firmware/components/webui/webui_page.h` | GPL-2.0-or-later (macemu) |
| Infinite Mac's favicon (mihaip/infinite-mac, `public/favicon32.png`) | `tools/webui/infinite-mac.png`, inlined in `firmware/components/webui/webui_page.h` | Apache-2.0 |
| ESP-IDF and its managed components (Espressif) | fetched at build time, not in this repository | Apache-2.0 |
