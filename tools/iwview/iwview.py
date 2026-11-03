#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws an ImageWriter II stream as the sheet that comes off the printer.

    python3.14 tools/iwview/iwview.py STREAM OUT.png [--dots DOTS.png] [--fonts DIR]

OUT.png is the page as ink on paper at 288 px an inch, in the settings page's
"press" model (tools/webui/app.js): a dot is as wide as a row is tall, and inks
take away what they absorb from the paper, so overprints mix. --dots writes the
raw grid, one pixel a dot position at 144 x 72 dpi. Only the first page is drawn.

Bit images are exact. Text needs the printer's own dot patterns: JSON files per
font (draft, corr, corr_prop, nlq, nlq_prop and their _mt MouseText sets), each
{"rows": 9, "glyphs": {"65": [column bitmasks, bit 0 the top dot], ...}}. They
come from the Technical Reference's Appendix C, which is Apple's, so they are
never committed: keep them in ~/.config/airwriter/iw-fonts/ or pass --fonts.
Without them text is drawn as boxes, the right size.

Covers what AirWriter's streams use: ESC c a m M p P n N E e q Q ! " X Y w W x
y z & $ K T A B F G S g V R s, CR, LF, FF, SO, SI; ESC Z, D and L are skipped.
Needs numpy and Pillow (python3.14 here).
"""
import argparse, json, os, sys

import numpy as np
from PIL import Image

PITCH = {  # code: (characters per inch, or None for proportional; dot columns per inch)
    'n': (9, 72), 'N': (10, 80), 'E': (12, 96), 'e': (13.4, 107.2),
    'q': (15, 120), 'Q': (17, 136), 'p': (None, 144), 'P': (None, 160),
}
Y, M, C, K = 1, 2, 4, 8
COLOUR = {0: K, 1: Y, 2: M, 3: C, 4: Y | M, 5: Y | C, 6: M | C}     # ESC K n
INKS = {Y: (250, 215, 0), M: (225, 30, 110), C: (0, 150, 210), K: (38, 36, 34)}
PAPER = (252, 250, 245)
FONTS = ('draft', 'corr', 'corr_prop', 'nlq', 'nlq_prop', 'draft_mt', 'corr_mt', 'nlq_mt')


# The press model, shared with tools/stylewriter/swfake.py (360 dots an inch): a
# sheet of grainy paper, ink laid on it dot by dot, each ink taking away what it
# absorbs, so overprints mix.
def blank_paper(W, H, rng):
    px = np.empty((H, W, 3), np.float32)
    px[:] = PAPER
    px += (rng.random((H, W, 1), np.float32) - .5) * 5
    return px


def dot_radius(ppi, dot_dpi=72):
    """A dot's radius in pixels: the printer's dot pitch is 1 / dot_dpi in."""
    return ppi / dot_dpi / 2


def pad_for(r):
    """The border `spread` needs round the hit counts for dots of radius r."""
    return int(np.ceil(r + 1))


def spread(cnt, r):
    """Ink cover of dots of radius r px. cnt is the hits (weight 0.82 to 1) per
    pixel, padded by pad_for(r) before and two more after; the cover is H x W."""
    R = pad_for(r)
    H, W = cnt.shape[0] - 2 * R - 2, cnt.shape[1] - 2 * R - 2
    cov = np.zeros((H, W), np.float32)
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            k = min(1.0, max(0.0, r + .5 - np.hypot(dx, dy)))
            if k > 0:
                cov += k * cnt[R - dy:R - dy + H, R - dx:R - dx + W]
    return cov


def lay_ink(px, cov, ink):
    px *= 1 - np.minimum(cov, 1.0)[..., None] * (1 - np.array(INKS[ink], np.float32) / 255)


def load_fonts(d):
    fonts = {}
    for name in FONTS:
        p = os.path.join(d, name + '.json')
        if os.path.exists(p):
            fonts[name] = {int(k): v for k, v in json.load(open(p))['glyphs'].items()}
    return fonts


class Printer:
    def __init__(self, data, fonts):
        self.data, self.fonts = data, fonts
        self.dots = []                      # (x in, y in, ink)
        self.y = 0                          # 1/144 in below top of form
        self.unknown = set()
        self.reset()

    def reset(self):                        # ESC c, with DIP switches at 10 cpi
        self.quality, self.pitch = 'draft', 'N'
        self.bold = self.ul = self.dw = self.hh = self.mt = False
        self.script, self.ink, self.lf, self.x, self.extra = None, K, 24, 0.0, 0

    def dot(self, x, y):
        for ink in (Y, M, C, K):
            if self.ink & ink:
                self.dots.append((x, y, ink))

    # Text. Draft has no bold, wide, half-height, script or proportional
    # forms: those print in correspondence (Technical Reference, ch. 4).
    def text(self, ch):
        cpi, dpi = PITCH[self.pitch]
        prop = cpi is None
        q = self.quality
        if self.hh or self.script or (q == 'draft' and (self.bold or self.dw or prop)):
            q = 'corr'
        if self.mt and 0x40 <= ch <= 0x5F:
            g = self.fonts.get(q + '_mt', {}).get(ch + 0x80)
        else:
            g = self.fonts.get(q + ('_prop' if prop else ''), {}).get(ch)
        if g is None:
            g = [0] * 8 if ch == 0x20 else [0x7F] * 7 + [0]
        if prop:
            step = 1.0 / dpi
            cell = len(g) * step + self.extra / dpi
        else:
            cell = 1.0 / cpi
            step = cell / len(g)
        if self.dw:
            step, cell = step * 2, cell * 2
        rows = 18 if q == 'nlq' and not (self.hh or self.script) else 9
        rstep = 1 / 144 if rows == 18 or self.hh or self.script else 1 / 72
        top = self.y / 144 + (3 / 72 if self.hh else 4 / 72 if self.script == 'sub' else 0)
        for ci, col in enumerate(g):
            for r in range(rows):
                if col >> r & 1:
                    x = self.x + ci * step
                    self.dot(x, top + r * rstep)
                    if self.dw: self.dot(x + step / 2, top + r * rstep)
                    if self.bold: self.dot(x + 1 / 144, top + r * rstep)
        if self.ul:
            uy = self.y / 144 + (6 / 72 if self.hh and not self.script else 8 / 72)
            for i in range(int(cell * 144)):
                self.dot(self.x + i / 144, uy)
        self.x += cell

    def column(self, b):                    # one graphics byte, bit 0 the top pin
        for r in range(8):
            if b >> r & 1:
                self.dot(self.x, self.y / 144 + r / 72)
        self.x += (2 if self.dw else 1) / PITCH[self.pitch][1]

    def num(self, i, n):
        return int(self.data[i:i + n].decode('ascii').replace(' ', '0')), i + n

    def run(self):
        d, i = self.data, 0
        flags = {'!': ('bold', True), '"': ('bold', False), 'X': ('ul', True), 'Y': ('ul', False),
                 'w': ('hh', True), 'W': ('hh', False), '&': ('mt', True), '$': ('mt', False)}
        while i < len(d):
            b = d[i]; i += 1
            if b == 0x1B:
                c = chr(d[i]); i += 1
                if c == 'c': self.reset()
                elif c in PITCH: self.pitch = c
                elif c == 'a': self.quality = {'0': 'corr', '1': 'draft', '2': 'nlq'}[chr(d[i])]; i += 1
                elif c == 'm': self.quality = 'corr'
                elif c == 'M': self.quality = 'nlq'
                elif c in flags: setattr(self, *flags[c])
                elif c in 'xyz': self.script = {'x': 'sup', 'y': 'sub', 'z': None}[c]
                elif c == 'K': self.ink = COLOUR[int(chr(d[i]))]; i += 1
                elif c == 'T': self.lf, i = self.num(i, 2)
                elif c == 'A': self.lf = 24
                elif c == 'B': self.lf = 18
                elif c == 'F':
                    n, i = self.num(i, 4)
                    self.x = n / PITCH[self.pitch][1]
                elif c in 'GS':
                    n, i = self.num(i, 4)
                    for b2 in d[i:i + n]: self.column(b2)
                    i += n
                elif c == 'g':
                    n, i = self.num(i, 3)
                    for b2 in d[i:i + n * 8]: self.column(b2)
                    i += n * 8
                elif c == 'V':
                    n, i = self.num(i, 4)
                    for _ in range(n): self.column(d[i])
                    i += 1
                elif c == 'R':
                    n, i = self.num(i, 3)
                    for _ in range(n): self.text(d[i])
                    i += 1
                elif c == 's': self.extra = int(chr(d[i])); i += 1
                elif c in 'ZD': i += 2
                elif c == 'L': i += 3
                else: self.unknown.add(c)
            elif b == 0x0D: self.x = 0.0
            elif b == 0x0A: self.y += self.lf
            elif b == 0x0C: break
            elif b == 0x0E: self.dw = True
            elif b == 0x0F: self.dw = False
            elif 0x20 <= b < 0x7F: self.text(b)
        return self

    def paper(self, path, ppi=288, left=0.25, dot_dpi=72):
        W, H = int(8.5 * ppi), int(11 * ppi)
        rng = np.random.default_rng(7)
        px = blank_paper(W, H, rng)
        r = dot_radius(ppi, dot_dpi)        # a dot is as wide as a row is tall
        R = pad_for(r)
        arr = np.array(self.dots, dtype=np.float64).reshape(-1, 3)
        for ink in (Y, M, C, K):
            sel = arr[arr[:, 2] == ink]
            if not len(sel): continue
            ix = np.rint((left + sel[:, 0]) * ppi).astype(int)
            iy = np.rint(sel[:, 1] * ppi).astype(int)
            ok = (ix >= 0) & (ix < W) & (iy >= 0) & (iy < H)
            cnt = np.zeros((H + 2 * R + 2, W + 2 * R + 2), np.float32)
            np.add.at(cnt, (iy[ok] + R, ix[ok] + R), 0.82 + 0.18 * rng.random(ok.sum()))
            lay_ink(px, spread(cnt, r), ink)
        Image.fromarray(px.clip(0, 255).astype(np.uint8)).save(path)

    def grid(self, path):
        px = np.full((792, 1152, 3), 255, np.float32)
        for x, y, ink in self.dots:
            c, r = int(round(x * 144)), int(round(y * 72))
            if 0 <= c < 1152 and 0 <= r < 792:
                px[r, c] *= np.array(INKS[int(ink)], np.float32) / 255
        Image.fromarray(px.clip(0, 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('stream')
    ap.add_argument('out')
    ap.add_argument('--dots')
    ap.add_argument('--fonts', default=os.path.expanduser('~/.config/airwriter/iw-fonts'))
    a = ap.parse_args()
    fonts = load_fonts(a.fonts)
    if not fonts:
        print(f'iwview: no fonts in {a.fonts}; text is drawn as boxes', file=sys.stderr)
    p = Printer(open(a.stream, 'rb').read(), fonts).run()
    p.paper(a.out)
    if a.dots: p.grid(a.dots)
    if p.unknown: print('iwview: skipped ESC', ' '.join(sorted(p.unknown)), file=sys.stderr)


if __name__ == '__main__':
    main()
