# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws scenes.c's frames as GIFs of the panel's dot grid, for the wiki.

usage: render.py FRAMES LAYOUT OUTDIR [--width PX] [--only NAME ...]

The look is the measured panel material model (docs: the dot-panel spec and
W-1049's previews): the FR4 dots crisp at full strength with a little glow; a
single-mask dot (B) passes ~10% and a double-mask dot (L) ~5%, both fuzzy and
with a green cast, so blue goes teal, yellow goes olive and red passes well.
The dots with no LED behind them are drawn dim, so a lit dot never reads as
one of the gold ones.
"""
import argparse, json, os, shutil, subprocess
import numpy as np
from PIL import Image, ImageFilter

STEP_MS = 40                    # scenes.c's STEP_MS
FULL = 64.0                     # 8-bit drive that reads as full white on screen (the caps are 48-192)
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

        # One coverage map per LED, on its layer: FR4 crisp, mask a little wider.
        self.fr4 = np.array([m == 'P' for _, _, m in chain])
        self.tr = np.array([1.0 if m == 'P' else 0.10 if m == 'B' else 0.05 for _, _, m in chain], np.float32)
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
        lin = lv / FULL
        # the mask's cast: blue goes teal, yellow goes olive, red passes
        cast = np.stack([lin[:, 0], lin[:, 1] + 0.35 * lin[:, 2], 0.55 * lin[:, 2]], 1) * (self.tr * 1.5)[:, None]
        fr4 = np.einsum('khw,kc->hwc', self.maps[self.fr4], lin[self.fr4])
        mask = np.einsum('khw,kc->hwc', self.maps[~self.fr4], cast[~self.fr4])
        f = Image.fromarray(screen(fr4))
        glow = np.asarray(f.filter(ImageFilter.GaussianBlur(self.glow_r)), np.float32) * 0.35
        haze = np.asarray(Image.fromarray(screen(mask)).filter(ImageFilter.GaussianBlur(self.haze_r)), np.float32)
        return np.maximum(self.base, np.asarray(f, np.float32)) + glow + haze * 1.2


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
    """One palette for the whole GIF, so frames diff well and a slow fade
    doesn't shimmer. 96 colours by area (the background and unlit dots
    exactly), 159 from the distinct colours each counted once and refined by
    k-means: by area alone, a mostly red scene spends the palette on red and
    tints Ready's small white dots."""
    px = np.concatenate([f.reshape(-1, 3) for f in imgs])
    by_area = Image.fromarray(px[::5][None]).quantize(96, method=Image.Quantize.MEDIANCUT)
    p = px.astype(np.uint32) >> 3
    key = np.unique(p[:, 0] << 10 | p[:, 1] << 5 | p[:, 2])
    uniq = (np.stack([key >> 10, key >> 5 & 31, key & 31], 1) << 3).astype(np.uint8)
    by_colour = Image.fromarray(uniq[None]).quantize(159, method=Image.Quantize.MEDIANCUT, kmeans=3)
    entries = by_area.getpalette()[:96 * 3] + by_colour.getpalette()[:159 * 3]
    pal = Image.new('P', (1, 1))
    pal.putpalette(entries + [0] * (768 - len(entries)))
    return pal


def save_gif(frames, path):
    imgs = [np.clip(f, 0, 255).astype(np.uint8) for f in frames]
    pal = palette(imgs)
    q = [Image.fromarray(f).quantize(palette=pal, dither=Image.Dither.NONE) for f in imgs]
    q[0].save(path, save_all=True, append_images=q[1:], duration=STEP_MS, loop=0, optimize=False)
    if shutil.which('gifsicle'):
        subprocess.run(['gifsicle', '-b', '-O3', path], check=True)


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
        save_gif(frames, path)
        print(f"{s['name']:20s} {len(frames) * STEP_MS / 1000:5.1f} s {os.path.getsize(path) // 1024:5d} KB")


if __name__ == '__main__':
    main()
