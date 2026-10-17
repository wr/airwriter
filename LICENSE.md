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
the original.

## Third-party material

| Item | Where | Licence / terms |
|---|---|---|
| TashTalk 2.1.3 firmware (lampmerchant) | `firmware/components/pic_prog/tashtalk-2.1.3.hex`, source alongside | GPL-3.0; see that folder's `NOTICE` |
| URF line decoding, ported from `_cupsRasterReadPixels()` in OpenPrinting CUPS `cups/raster-stream.c` | `firmware/components/urf_decode/urf_decode.c` | Apache-2.0 (CUPS); compatible with GPL-3.0 |
| WS2812B-2020 3D model, KiCad libraries | `hardware/airwriter/lib/airwriter.3dshapes/LED_WS2812B-2020_PLCC4_2.0x2.0mm.step` | CC-BY-SA-4.0 with the KiCad libraries exception |
| Kycon KMDGX-8S-BS 3D model, from Kycon | `hardware/airwriter/lib/airwriter.3dshapes/MiniDIN-8_Kycon_KMDGX-8S-BS_RightAngle.step` | Manufacturer's model, redistributed for design use |
| Apple's colour ImageWriter icon, redrawn pixel for pixel, used as the printer's icon | `tools/icon/imagewriter.txt`, `firmware/components/ipp_server/icons/` | © Apple; not licensed under this repository's licences |
| ESP-IDF and its managed components (Espressif) | fetched at build time, not in this repository | Apache-2.0 |
