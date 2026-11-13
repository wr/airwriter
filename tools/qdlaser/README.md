# QuickDraw LaserWriter tools

Tools for the Personal LaserWriter 300 on the printer port (`W-1102`). The design is
`docs/superpowers/specs/2026-11-12-quickdraw-laserwriters-design.md`; the firmware is
`firmware/components/qdlaser`.

- `qdfake.py` is a Personal LaserWriter 300 in software. It answers packets the way the design's
  command table says the printer does, decodes the pages it gets with its own decoder, and saves
  each one as a PBM and a PNG. `serve --pty` waits on a pseudo-terminal, with packets framed
  because a pty cannot carry the printer's clock; lines on its stdin make it run out of paper,
  jam, open its cover or go silent. `decode` and `decode-bits` read a file that
  `rip --printer plw300` wrote: the framed packets, or the line itself as the board clocks it
  out.
- `test_qdfake.py` holds `qdfake.py` to packets and a line image built by hand from the design.
  Run it with `python3 -m unittest tools/qdlaser/test_qdfake.py`.
- `test_qdinterop.py` runs `tools/rip/build/qdhost` (the firmware's session, page stage and rip
  built for this computer) against `qdfake.py`: pages, faults, the manual slot, a cancel, a
  printer switched off, a Personal LaserWriter LS. Run it with `make -C tools/rip qdinterop`.
  It takes about 10 seconds.
- `mkqdtables.py` reads Apple's 300 screen, its tone curve and the printer's icon out of Apple's
  LaserWriter 300 driver and writes them as source. The driver is never committed.

`make -C tools/rip qdcheck` checks the firmware's packets and line image against `qdfake.py`
and the rip's own dots.
