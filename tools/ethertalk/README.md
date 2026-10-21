<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# etpeer

An EtherTalk Mac in software (W-1076): it takes an AppleTalk address with
AARP, finds the board with an NBP lookup as the Chooser does, and prints a file
over PAP as Apple's AppleTalk ImageWriter driver does. It reaches the board the
two ways an emulated Mac does: Basilisk II's UDP tunnel on this network, or an
Infinite Mac zone.

```sh
python3 tools/ethertalk/etpeer.py tunnel lookup
python3 tools/ethertalk/etpeer.py tunnel print fixtures/pap-iw701-page.bin
python3 tools/ethertalk/etpeer.py zone ZONE lookup     # needs: pip install websockets
python3 tools/ethertalk/etpeer.py zone ZONE print FILE
```

`print` makes the ImageWriter print: ask whoever is near it first. The file
goes to the printer as it is, so it must be an ImageWriter stream, such as
`fixtures/pap-iw701-page.bin` (a real Mac's page, crc32 `62416521`); compare
the board's "job printed" line in `/api/et`. The board's address defaults to
10.0.1.115 (`AIRWRITER=` to change it). The tunnel binds UDP 6066, so it
cannot run beside Basilisk II on the same computer.
