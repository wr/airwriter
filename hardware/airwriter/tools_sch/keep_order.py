#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Put a konnect edit of the schematic back on the committed file's text and order.

    git show HEAD:hardware/airwriter/airwriter.kicad_sch > old.kicad_sch
    kicad-cli sch upgrade --force hardware/airwriter/airwriter.kicad_sch   # the edited file
    python3 tools_sch/keep_order.py old.kicad_sch hardware/airwriter/airwriter.kicad_sch out.kicad_sch

konnect writes the whole schematic back in an order of its own, and KiCad keeps
whatever order it reads, so an edit of a few parts shows as a rewrite of the file.
This keeps OLD's text for every item NEW leaves unchanged (matched by uuid, a lib
symbol by name; compared ignoring the fields KiCad writes out as defaults, pin
uuids and the order of an item's own fields), takes NEW's text for one it changed,
drops one it deleted, and adds one it added after OLD's last item of the same
kind. The diff is then the change. Check that out.kicad_sch exports the same
netlist as NEW before copying it over the schematic (W-1176 did).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse  # noqa: E402

DEFAULTS = {'stroke', 'effects', 'show_name', 'do_not_autoplace', 'in_pos_files', 'body_style',
            'exclude_from_sim', 'duplicate_pin_numbers_are_jumpers', 'fields_autoplaced', 'pin'}
UUID_KINDS = ('symbol', 'label', 'global_label', 'hierarchical_label', 'wire', 'junction',
              'no_connect', 'text', 'sheet', 'bus', 'bus_entry', 'polyline', 'rectangle')


def children(t, start, end):
    """Spans (a, b) of the lists directly inside t[start:end]: a at '(', b after ')'."""
    out, i, depth, a, instr = [], start, 0, None, False
    while i < end:
        c = t[i]
        if instr:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == '(':
            if depth == 0:
                a = i
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                out.append((a, i + 1))
        i += 1
    return out


def relaxed(text):
    def walk(x):
        if isinstance(x, list):
            if x and str(x[0]) in DEFAULTS:
                return None
            kids = [w for w in (walk(i) for i in x[1:]) if w is not None]
            return (walk(x[0]),) + tuple(sorted(kids, key=repr))
        s = str(x)
        try:
            return round(float(s), 4)
        except ValueError:
            return s
    return walk(parse(text)[0])


def head(text):
    m = re.match(r'\((\S+?)(?:\s+"([^"]*)")?[\s)]', text)
    return m.group(1), m.group(2)


def key(text):
    h, name = head(text)
    if h == 'symbol' and name is not None:          # a lib symbol
        return ('sym', name)
    if h in UUID_KINDS:
        for a, b in children(text, 1, len(text) - 1):
            m = re.match(r'\(uuid\s+"([^"]+)"\)', text[a:b])
            if m:
                return ('uuid', m.group(1))
    return ('head', h)


def kind(k, text):
    return head(text)[0] if k[0] == 'uuid' else k[0]


def merge(ot, nt):
    """ot, nt: the text of one list, parens included. Returns ot with nt's changes."""
    os_, ns = children(ot, 1, len(ot) - 1), children(nt, 1, len(nt) - 1)
    new = {key(nt[a:b]): nt[a:b] for a, b in ns}
    items, seen, pos = [], set(), 1               # (kind, text, whitespace before it)
    for a, b in os_:
        sep, pos = ot[pos:a], b
        k = key(ot[a:b])
        if k not in new:
            continue                                # deleted
        seen.add(k)
        o, n = ot[a:b], new[k]
        txt = merge(o, n) if k == ('head', 'lib_symbols') else (o if relaxed(o) == relaxed(n) else n)
        items.append((kind(k, o), txt, sep))
    for a, b in ns:
        k = key(nt[a:b])
        if k in seen:
            continue                                # added: after OLD's last of its kind
        kd = kind(k, nt[a:b])
        idx = max([i for i, it in enumerate(items) if it[0] == kd], default=len(items) - 1)
        items.insert(idx + 1, (kd, nt[a:b], items[idx][2] if items else '\n\t'))
    return ot[:1] + ''.join(sep + txt for _, txt, sep in items) + ot[pos:]


def main():
    old, new = open(sys.argv[1]).read(), open(sys.argv[2]).read()
    (oa, ob), (na, nb) = children(old, 0, len(old))[0], children(new, 0, len(new))[0]
    open(sys.argv[3], 'w').write(old[:oa] + merge(old[oa:ob], new[na:nb]) + old[ob:])


if __name__ == '__main__':
    main()
