"""Cut a fit-test coupon out of a part: the part intersected with a box round
the feature being tuned, written to out/coupons/<part>.stl (gitignored,
unlike out/*.stl), so a 0.2 mm
change prints in minutes instead of a whole part (docs/cad-routine.md).
Case frame, mm; Z defaults to the part's full height.

    .venv/bin/python tools/coupon.py lattice 25 37 67 80
    .venv/bin/python tools/coupon.py tray -3.5 4 15 45 -7 15
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from build123d import Box, Pos, export_stl  # noqa: E402

import export  # noqa: E402


def main(argv):
    if len(argv) not in (5, 7):
        sys.exit(__doc__)
    name, *nums = argv
    builders = {k.removeprefix("airwriter-"): v for k, v in export.PARTS.items()}
    if name not in builders:
        sys.exit(f"part must be one of {', '.join(builders)}")
    part = builders[name]()
    x0, x1, y0, y1 = map(float, nums[:4])
    bb = part.bounding_box()
    z0, z1 = map(float, nums[4:]) if len(nums) == 6 else (bb.min.Z - 1, bb.max.Z + 1)
    cut = part & (Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0))
    if cut is None or cut.volume < 1e-6:
        sys.exit("the box misses the part")
    out = export.OUT / "coupons" / f"{name}.stl"
    out.parent.mkdir(parents=True, exist_ok=True)
    export_stl(cut, str(out))
    c = cut.bounding_box()
    print(f"{out}: {c.size.X:.1f} x {c.size.Y:.1f} x {c.size.Z:.1f} mm, {cut.volume / 1000:.2f} cm3")


if __name__ == "__main__":
    main(sys.argv[1:])
