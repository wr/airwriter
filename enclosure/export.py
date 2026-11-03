"""Write the parts to out/ as STEP (for the KiCad interference check) and
STL (to slice)."""
import pathlib
import sys

from build123d import export_step, export_stl

import case
import lattice
import lc_frame
import lc_light

OUT = pathlib.Path(__file__).resolve().parent / "out"

PARTS = {
    "airwriter-tray": case.tray,
    "airwriter-lc-lid": case.lc_lid,
    "airwriter-lc-frame": lc_frame.lc_frame,
    "airwriter-lc-light": lc_light.lc_light,
    "airwriter-lattice": lattice.lattice,
}


def main() -> int:
    OUT.mkdir(exist_ok=True)
    for name, build in PARTS.items():
        part = build()
        export_step(part, str(OUT / f"{name}.step"))
        export_stl(part, str(OUT / f"{name}.stl"))
        bb = part.bounding_box()
        print(f"{name}: {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, "
              f"{part.volume / 1000:.1f} cm3")
        if name == "airwriter-lattice":
            # Accepted bleed (a lit cell into a gold cell or the front), for a look.
            for leak in lattice.leaky_walls(part):
                print(f"  notched wall: {leak}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
