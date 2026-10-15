"""Render enclosure STLs to a PNG, so a change can be looked at before it is
printed (docs/cad-routine.md). Case frame, mm.

    .venv/bin/python tools/render.py out/airwriter-lattice.stl /tmp/lattice.png
    .venv/bin/python tools/render.py out/airwriter-lattice.stl /tmp/collar.png --box 25 37 67 80 --z 0 13.5

Several STLs overlay (the first grey, the rest translucent blue). --box crops
to an XY window and switches to close-up views. Crops are by triangle centre,
so long thin triangles at a crop edge can show as spikes: artefacts, not
geometry. Needs tools/requirements.txt in the venv."""
import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402
from stl import mesh  # noqa: E402

FULL = [("top", 90, -90), ("underside", -90, -90), ("iso", 35, -120), ("iso, from below", -35, -60)]
CLOSE = [("top", 75, -90), ("iso", 35, -120), ("iso, other side", 35, 60), ("from below", -35, -60)]


def _draw(ax, meshes, box, zl, el, az, title):
    x0, x1, y0, y1 = box
    for i, v in enumerate(meshes):
        c = v.mean(axis=1)
        v = v[(c[:, 0] > x0) & (c[:, 0] < x1) & (c[:, 1] > y0) & (c[:, 1] < y1)]
        n = np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
        n /= np.linalg.norm(n, axis=1)[:, None] + 1e-12
        e, a = np.radians(el), np.radians(az)
        light = np.array([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)]) + [0.3, 0.2, 0.6]
        shade = np.clip(0.25 + 0.75 * np.abs(n @ (light / np.linalg.norm(light))), 0, 1)
        base, alpha = ((0.8, 0.8, 0.8), 0.95) if i == 0 else ((0.55, 0.85, 1.0), 0.6)
        rgba = np.c_[np.array(base)[None, :] * shade[:, None], np.full(len(v), alpha)]
        ax.add_collection3d(Poly3DCollection(v, facecolors=rgba, edgecolors="none"))
    ax.set_xlim(x0, x1); ax.set_ylim(y0, y1); ax.set_zlim(*zl)
    ax.set_box_aspect((x1 - x0, y1 - y0, zl[1] - zl[0]))
    ax.view_init(el, az); ax.set_title(title); ax.set_axis_off()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("stl", nargs="+", help="STL files, then the output PNG last")
    ap.add_argument("--box", nargs=4, type=float, metavar=("X0", "X1", "Y0", "Y1"))
    ap.add_argument("--z", nargs=2, type=float, metavar=("Z0", "Z1"))
    a = ap.parse_args()
    *stls, out = a.stl
    meshes = [mesh.Mesh.from_file(s).vectors for s in stls]
    pts = np.concatenate([m.reshape(-1, 3) for m in meshes])
    lo, hi = pts.min(0), pts.max(0)
    box = a.box or (lo[0] - 1, hi[0] + 1, lo[1] - 1, hi[1] + 1)
    zl = a.z or (lo[2] - (hi[2] - lo[2]), hi[2] + (hi[2] - lo[2]))
    fig = plt.figure(figsize=(16, 12))
    for k, (title, el, az) in enumerate(CLOSE if a.box else FULL):
        _draw(fig.add_subplot(2, 2, k + 1, projection="3d"), meshes, box, zl, el, az, title)
    plt.tight_layout()
    plt.savefig(out, dpi=85)
    print(out)


if __name__ == "__main__":
    main()
