# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws scenes.c's frames as GIFs of the panel's dot grid, for the wiki.

usage: render.py FRAMES LAYOUT OUTDIR [--width PX] [--only NAME ...]

Each dot is drawn in the colour board 1's panel was photographed showing
(panel_colours.json: 40 colours on every kind of dot, 2026-10-17). A dot's
drive goes back through the firmware's gamma to a hue, saturation and level;
the hue picks between the 24 measured hues, the saturation mixes in the
measured white, and the level scales the light. So blue reads sky blue only at
hue 240, purple comes out lavender or pink, and white turns mint under the
mask, as on the panel. The FR4 dots are crisp with a little glow; the mask
dots (B single, L double) are fuzzy. The dots with no LED behind them are drawn
dim, so a lit dot never reads as one of the gold ones. The corners are rounded
by transparency (GIF has no partial alpha), so they sit on a light or a dark
page alike.
"""
import argparse, colorsys, json, os, shutil, subprocess
import numpy as np
from PIL import Image, ImageFilter

STEP_MS = 40                    # scenes.c's STEP_MS
CAP = {'P': 48.0, 'B': 192.0, 'L': 192.0}      # led_grid_core.h's caps: a level of 1
BG = (14, 14, 16)
UNLIT = {'G': (58, 48, 24), 'T': (58, 48, 24), 'P': (36, 33, 27), 'B': (20, 24, 22), 'L': (16, 18, 17)}


def read_frames(path):
    chain, scenes, cur = None, [], None
    for ln in open(path):
        if ln.startswith('M '):
            chain = [(int(r), int(c), m) for r, c, m in (t.split(',') for t in ln[2:].split())]
        elif ln.startswith('P '):
            name, loop = ln[2:].split()
            cur = {'name': name, 'xfade': 0 if loop == 'cut' else int(loop), 'frames': []}
            scenes.append(cur)
        else:
            cur['frames'].append(np.array(ln.split(), np.float32).reshape(-1, 3) / 256.0)
    return chain, scenes


class Panel:
    def __init__(self, layout, chain, width):
        lay = json.load(open(layout))
        g = lay['grid']
        gw = lay['panel']['w']
        gh = g['origin'][1] * 2 + (g['rows'] - 1) * g['pitch']
        self.sc = sc = width / gw                       # px per mm
        self.W, self.H = width, int(round(gh * sc))
        yy, xx = np.mgrid[:self.H, :self.W].astype(np.float32)
        dots = {(d['r'], d['c']): d for d in lay['dots']}
        rad = g['dot_dia'] * sc / 2

        def disc(x, y, r):                               # anti-aliased, 1.5 px edge
            return np.clip((r - np.hypot(xx - x, yy - y)) / 1.5 + 0.5, 0, 1)

        base = np.zeros((self.H, self.W, 3), np.float32) + BG
        for d in lay['dots']:
            a = disc(d['x'] * sc, d['y'] * sc, rad)[..., None]
            base = base * (1 - a) + a * np.array(UNLIT[d['mat']], np.float32)
            if d['mat'] == 'T':
                a = disc(d['x'] * sc, d['y'] * sc, g['hole_dia'] * sc / 2)[..., None]
                base = base * (1 - a) + a * 6
        self.base = base
        # Rounded corners, radius a twentieth of the width: transparent outside.
        cr = width / 20
        cx = np.clip(xx + 0.5, cr, self.W - cr)
        cy = np.clip(yy + 0.5, cr, self.H - cr)
        self.outside = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy) > cr

        # One coverage map per LED, on its layer: FR4 crisp, mask a little wider.
        self.mat = [m for _, _, m in chain]
        self.fr4 = np.array([m == 'P' for m in self.mat])
        maps = []
        for r, c, m in chain:
            d = dots[(r, c)]
            k = 1.0 if m == 'P' else 1.25 if m == 'B' else 1.05
            maps.append(disc(d['x'] * sc, d['y'] * sc, rad * k))
        self.maps = np.stack(maps)                      # (37, H, W)
        self.glow_r = 4 * sc / 5
        self.haze_r = 5 * sc / 5

    def draw(self, lv):
        """lv: (37, 3) 8-bit drive levels -> (H, W, 3) float screen RGB."""
        col = np.stack([look(lv[i], m) for i, m in enumerate(self.mat)])
        fr4 = np.einsum('khw,kc->hwc', self.maps[self.fr4], col[self.fr4])
        mask = np.einsum('khw,kc->hwc', self.maps[~self.fr4], col[~self.fr4])
        f = Image.fromarray(screen(fr4))
        glow = np.asarray(f.filter(ImageFilter.GaussianBlur(self.glow_r)), np.float32) * 0.35
        haze = np.asarray(Image.fromarray(screen(mask)).filter(ImageFilter.GaussianBlur(self.haze_r * 0.7)), np.float32)
        return np.maximum(self.base, np.asarray(f, np.float32)) + glow + haze * 0.9


def linear(srgb):
    c = np.asarray(srgb, np.float32) / 255
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


# The measured colours, per material: the 24-hue ring at full saturation and
# the neutral white, in linear light.
COLOURS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'panel_colours.json')))
RING = {m: np.stack([linear(e[m]) for e in COLOURS[:24]]) for m in 'PBL'}
WHITE = {m: linear(next(e[m] for e in COLOURS if e['name'] == 'neutral')) for m in 'PBL'}


def look(drive, mat):
    """One dot's 8-bit drive -> its measured colour in linear light."""
    if drive.max() < 0.5:
        return np.zeros(3, np.float32)
    p = (np.maximum(drive, 0) / CAP[mat]) ** (1 / 2.2)     # back to the firmware's levels
    v = p.max()
    h, s, _ = colorsys.rgb_to_hsv(*(p / v))
    x = h * 24 % 24
    i, f = int(x), x % 1
    hue = RING[mat][i] * (1 - f) + RING[mat][(i + 1) % 24] * f
    w = (1 - s) ** 2.2                                      # the white in it, as light
    return (hue * (1 - w) + WHITE[mat] * w) * v ** 2.2


def screen(lin):
    return (np.clip(lin, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def loop(frames, xfade_ms):
    """Joins the end to the start: the last n frames fade into the first n."""
    n = xfade_ms // STEP_MS
    if not n:
        return frames
    keep = len(frames) - n
    out = list(frames[:keep])
    for i in range(n):
        k = (i + 0.5) / n
        out[i] = frames[keep + i] * (1 - k) + frames[i] * k
    return out


def palette(imgs):
    """One palette for a run of frames, so they diff well and a slow fade
    doesn't shimmer. 96 colours by area (the background and unlit dots
    exactly), 159 from the distinct colours each counted once and refined by
    k-means: by area alone, a mostly red stretch spends the palette on red and
    tints Ready's small white dots."""
    px = np.concatenate([f.reshape(-1, 3) for f in imgs])
    by_area = Image.fromarray(px[::5][None]).quantize(96, method=Image.Quantize.MEDIANCUT)
    p = px.astype(np.uint32) >> 3
    key = np.unique(p[:, 0] << 10 | p[:, 1] << 5 | p[:, 2])
    uniq = (np.stack([key >> 10, key >> 5 & 31, key & 31], 1) << 3).astype(np.uint8)
    by_colour = Image.fromarray(uniq[None]).quantize(159, method=Image.Quantize.MEDIANCUT, kmeans=3)
    entries = by_area.getpalette()[:96 * 3] + by_colour.getpalette()[:159 * 3]
    return entries + [0] * (765 - len(entries))


PAL_FRAMES = 50     # a fresh palette every 2 s: one for a whole 16 s scene turns Ready pink


def save_gif(frames, outside, path):
    imgs = [np.clip(f, 0, 255).astype(np.uint8) for f in frames]
    q = []
    for c0 in range(0, len(imgs), PAL_FRAMES):
        run = imgs[c0:c0 + PAL_FRAMES]
        entries = palette(run) + [0, 0, 0]      # entries 0-254; 255 is the clear corner
        pal = Image.new('P', (1, 1))
        pal.putpalette(entries)
        for f in run:
            a = np.asarray(Image.fromarray(f).quantize(palette=pal, dither=Image.Dither.NONE)).copy()
            a[outside] = 255
            im = Image.fromarray(a, 'P')
            im.putpalette(entries)
            q.append(im)
    q[0].save(path, save_all=True, append_images=q[1:], duration=STEP_MS, loop=0, optimize=False,
              transparency=255, disposal=1)
    if shutil.which('gifsicle'):
        subprocess.run(['gifsicle', '-b', '-O3', path], check=True, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames')
    ap.add_argument('layout')
    ap.add_argument('outdir')
    ap.add_argument('--width', type=int, default=240)
    ap.add_argument('--only', nargs='*')
    a = ap.parse_args()
    chain, scenes = read_frames(a.frames)
    panel = Panel(a.layout, chain, a.width)
    os.makedirs(a.outdir, exist_ok=True)
    for s in scenes:
        if a.only and s['name'] not in a.only:
            continue
        frames = loop([panel.draw(f) for f in s['frames']], s['xfade'])
        path = os.path.join(a.outdir, s['name'] + '.gif')
        save_gif(frames, panel.outside, path)
        print(f"{s['name']:20s} {len(frames) * STEP_MS / 1000:5.1f} s {os.path.getsize(path) // 1024:5d} KB")


if __name__ == '__main__':
    main()
