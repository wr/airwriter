#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refuses a release that would leave sold boards able to go back to firmware
without sign-in (W-1156; SPEC §3.12). A board with a password won't take an
image older than FWU_SIGNIN_FLOOR once it runs that version or later, so the
floor must be the first release with sign-in: no release may come in below it,
and no release from before sign-in may sit at or past it.

    python3 tools/release/floor_check.py vX.Y.Z      (release.sh runs it)
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REL = "firmware/components/fw_update/include/fw_update_core.h"
HEADER = ROOT / REL


def ver(s):
    return tuple(int(x) for x in s[1:].split("."))


def floor_of(header_text):
    m = re.search(r'#define\s+FWU_SIGNIN_FLOOR\s+"(v\d+\.\d+\.\d+)"', header_text)
    return m.group(1) if m else None


def problems(tag, floor, tags_without_signin):
    out = []
    if ver(tag) < ver(floor):
        out.append("%s is below FWU_SIGNIN_FLOOR (%s). The first release with sign-in sets the "
                   "floor to its own tag." % (tag, floor))
    for t in tags_without_signin:
        if ver(t) >= ver(floor):
            out.append("%s has no sign-in but sits at or past FWU_SIGNIN_FLOOR (%s): a board could "
                       "go back to it and open its network. Raise the floor to this release's tag."
                       % (t, floor))
    return out


def tags_without_signin():
    tags = subprocess.run(["git", "-C", str(ROOT), "tag", "-l", "v*"], capture_output=True,
                          text=True, check=True).stdout.split()
    out = []
    for t in tags:
        if not re.fullmatch(r"v\d+\.\d+\.\d+", t):
            continue
        r = subprocess.run(["git", "-C", str(ROOT), "show", "%s:%s" % (t, REL)], capture_output=True,
                           text=True)
        if r.returncode or not floor_of(r.stdout):
            out.append(t)
    return out


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r"v\d+\.\d+\.\d+", sys.argv[1]):
        sys.exit("usage: floor_check.py vX.Y.Z")
    floor = floor_of(HEADER.read_text())
    if not floor:
        sys.exit("floor_check: no FWU_SIGNIN_FLOOR in " + REL)
    p = problems(sys.argv[1], floor, tags_without_signin())
    for line in p:
        print("floor_check: " + line, file=sys.stderr)
    sys.exit(1 if p else 0)


if __name__ == "__main__":
    main()
