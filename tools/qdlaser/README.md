# QuickDraw LaserWriter tools

Tools for the Personal LaserWriter 300 (`W-1102`) and LS (`W-1145`) on the printer port. The design is
`docs/superpowers/specs/2026-11-12-quickdraw-laserwriters-design.md`; the firmware is
`firmware/components/qdlaser`.

- `qdfake.py` is a Personal LaserWriter 300 in software. It answers packets the way the design's
  command table says the printer does, decodes the pages it gets with its own decoder, and saves
  each one as a PBM and a PNG. `serve --pty` waits on a pseudo-terminal, with packets framed
  because a pty cannot carry the printer's clock; lines on its stdin make it run out of paper,
  jam, open its cover or go silent. `decode` and `decode-bits` read a file that
  `rip --printer plw300` wrote: the framed packets, or the line itself as the board clocks it
  out. `--engine 0d` makes it a Personal LaserWriter LS as the LS's own driver has it (no LS
  has been tried); it logs anything that driver never sends.
- `test_qdfake.py` holds `qdfake.py` to packets and a line image built by hand from the design.
  Run it with `python3 -m unittest tools/qdlaser/test_qdfake.py`.
- `test_qdinterop.py` runs `tools/rip/build/qdhost` (the firmware's session, page stage and rip
  built for this computer) against `qdfake.py`: pages, faults, the manual slot, a cancel, a
  printer switched off, and an LS's pages, faults and its refusal of `83 40`. Run it with
  `make -C tools/rip qdinterop`. It takes about 35 seconds.
- `mkqdtables.py` reads Apple's 300 screen and its tone curve out of Apple's LaserWriter 300
  driver, and the LS's icon out of its own driver, and writes them as source.
  The drivers are never committed.

`make -C tools/rip qdcheck` checks the firmware's packets and line image against `qdfake.py`
and the rip's own dots.
