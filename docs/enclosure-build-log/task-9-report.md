# Task 9 report: the carried-over corner test + the shop GLB

## Part 1: corner wall-thickness test

Added `TestCornerRounding.test_corner_fillets_are_concentric_with_2_80_radial_wall`
in `enclosure/test_case.py` (after the existing `TestCornerRounding` tests from
Task 10). Unlike the existing corner tests, which probe volume near each corner
with margin ("is there material/void here"), this one reads the fillet faces
back as real OCC cylinder geometry:

```python
cyl_faces = self.shell.faces().filter_by(GeomType.CYLINDER)
```

`case.shell()` has exactly 8 cylindrical faces: 4 exterior fillets (radius
`EXT_R`) and 4 pocket fillets (radius `POCKET_R`), one pair per corner. Each
face's `geom_adaptor().Cylinder()` gives an axis (`.Axis()` -> `Location()` +
`Direction()`) and a radius. The test:

1. Groups the 8 faces into 4 corners by rounding each axis's `(Location().X(),
   Location().Y())` to 3 places (Z is an arbitrary point on an infinite
   vertical line, not meaningful, so it's excluded from the key). Confirmed by
   inspection first — printing the raw axis locations showed the ext/pocket
   pair at each corner share the same (X, Y) to ~1e-9, so this grouping is not
   fuzzy matching dressed up, it's exact within float noise.
2. Asserts each group has exactly 2 faces.
3. Asserts the two faces' axis directions and (X, Y) locations agree to 1e-6 —
   "concentric."
4. Asserts `abs(r0 - r1) == p.WALL - p.POCKET_CLEAR` (2.80mm) to 4 places.

### Red/green evidence

Green today: `EXT_R = round(CORNER_R + WALL, 6)` = **4.999999**,
`POCKET_R = round(CORNER_R + POCKET_CLEAR, 6)` = **2.199999**, both sharing
`CORNER_R` = 1.999999. Ran the single new test:

```
test_corner_fillets_are_concentric_with_2_80_radial_wall ... ok
Ran 1 test in 0.171s
OK
```

Broke it: edited `params.py` so `EXT_R = round(CORNER_R + WALL - 0.5, 6)`
(**4.499999** instead of 4.999999 — a stand-in for EXT_R losing its shared
`CORNER_R` term). Reran the same test:

```
FAIL: test_corner_fillets_are_concentric_with_2_80_radial_wall
AssertionError: 8 != 4 : expected the tray's 4 rounded corners, found fillet
faces grouped into 8: [(1.5, 1.5), (1.5, 102.77), (2.0, 2.0), (2.0, 102.27),
(60.0, 2.0), (60.0, 102.27), (60.5, 1.5), (60.5, 102.77)]
```

It fails on the grouping assertion rather than the radial-wall assertion,
because changing `EXT_R` also moves the fillet's arc *centre* (a Box's vertical
edge filleted to radius R has its arc centre R in from the corner), so the
ext/pocket axes stop lining up at all, not just their radii. That's still the
right kind of red: the axes genuinely no longer coincide, which is exactly what
"the two fillet faces are no longer built from the same corner logic" looks
like at the geometry level. Restored `EXT_R` to `round(CORNER_R + WALL, 6)`
immediately after. `git diff enclosure/params.py` is clean in the committed
state — the break-and-restore never touched the commit.

Full suite after restoring: **65 tests, OK** (`./enclosure/build.sh`), matching
the 64 from Tasks 1-8/10 plus this one new test.

## Part 2: the shop GLB

### Build

```
cd /Users/wells/Projects/airwriter && ./enclosure/build.sh
```
```
airwriter-tray: 68.00 x 110.27 x 22.94 mm, 36.0 cm3
airwriter-lite-lid: 62.40 x 104.67 x 15.94 mm, 11.9 cm3
```

### Converter command

```
cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
  ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
  public/models/airwriter-case.glb \
  --name case --color 0.025,0.025,0.025 --rough 0.8 --metal 0 --lay-flat
```

Then, following `scripts/addon-models/build.sh`'s own post-conversion step for
every other add-on GLB (never `gltf-transform optimize`, which strips node
names the viewer relies on):

```
node scripts/board-model/tools/join-prims.mjs public/models/airwriter-case.glb public/models/airwriter-case.glb
npx @gltf-transform/cli dedup    public/models/airwriter-case.glb public/models/airwriter-case.glb
npx @gltf-transform/cli prune    public/models/airwriter-case.glb public/models/airwriter-case.glb
npx @gltf-transform/cli weld     public/models/airwriter-case.glb public/models/airwriter-case.glb
npx @gltf-transform/cli meshopt  public/models/airwriter-case.glb public/models/airwriter-case.glb
```

`join-prims.mjs` needed `@gltf-transform/functions`, which isn't installed
under `scripts/board-model/tools/` (no `package.json` there at all — it
apparently expects to run with the repo's dependency tree, which doesn't carry
gltf-transform at the root either). I ran `npm i` in `scripts/addon-models/`
(per its own header comment: "Needs `npm i` in this directory once"), then
temporarily symlinked `scripts/board-model/tools/node_modules ->
../../addon-models/node_modules` to run `join-prims.mjs` and the verification
tools (`bounds.mjs`, `nodeinfo.mjs`), and removed the symlink immediately after
(`scripts/addon-models/node_modules/` is gitignored by the repo's own
`.gitignore`, which already anticipates this exact symlink at
`scripts/board-model/tools/node_modules/`). No files under `scripts/` were
edited — `join-prims.mjs` is a genuine no-op here anyway (the STL→GLB output
has exactly one primitive already), so the size drop is entirely
dedup/prune/weld/meshopt. Neither `stl2glb.mjs` nor any other converter script
was modified.

### Why these options

- **`--color 0.025,0.025,0.025 --rough 0.8 --metal 0`**: matches
  `scripts/addon-models/build.sh`'s existing convention for the shop's other
  matte-black FDM part, the `maclock-screen-bezel` ("black PLA"), verbatim —
  same three numbers, same comment logic ("0.025 grey: the viewer's room light
  lifts dark plastics; this reads as black PLA on the page"). The AirWriter
  case's own README says "FDM ... matte black," so the same house convention
  applies rather than inventing a new one. `--color` is linear per the
  converter's header — 0.025 linear is well below what a naive sRGB 0x06 guess
  would give, which is the whole point of the existing comment.
- **`--lay-flat`, no `--flip`**: The tray is modelled in `case.py`'s frame with
  +Z toward the open top (board pocket, wall-top rim) and prints floor-down at
  model -Z (`test_printable.py`'s module docstring: "model -Z is down on the
  bed"). `export.py` writes the STL straight off `case.tray()` with no
  reorientation, so the STL is already in that same Z-up-is-open-top frame.
  `stl2glb.mjs`'s `orient()` with `--lay-flat` alone maps old Z -> new Y (and
  old Y -> new -Z); with no `--flip` there's no extra 180 deg about X. So model
  +Z (open top) becomes glTF +Y (up), and model -Z (closed floor slab) becomes
  the low side that rests on the ground plane — exactly "the open top with the
  ports, not the underside." Checked two ways before committing to it:
  1. **Traced the math** in `stl2glb.mjs`'s `orient()` (read its source,
     didn't guess) against `export.py`'s "no reorientation" behavior.
  2. **Probed the geometry independently**, before it ever reached the
     converter: sliced `case.tray()` with a thin (0.05mm) box 0.3mm off each Z
     extreme and measured the filled fraction against the full EXT_W x EXT_L
     footprint. Near the bottom (floor): 98.7% filled — a solid slab, as
     expected for the closed underside. Near the top: 12.7% filled — a thin
     rim, as expected for the open pocket. This confirms which end is "the
     open top with the ports" independent of any assumption about how
     `stl2glb.mjs`'s math works, and it matches (1).
  3. **Confirmed in the finished GLB** with
     `scripts/board-model/tools/nodeinfo.mjs`: node `case` has
     `translation.y = 11.470mm` (= half of 22.94mm, i.e. the mesh spans
     y in [0, 22.94] after `ground()`), local bounds scaled out to
     68.00 x 22.94 x 110.27mm in (x, y, z) — y is the up axis and the part
     rests on the ground plane, matching the tray's real footprint (68 x
     110.27mm) and height (22.94mm). I did not independently re-derive from
     the mesh triangles which face is visually "up" inside the GLB itself
     (no headless renderer was used) — the up-axis/ground-plane placement and
     the pre-conversion slice probe are what the "which side faces up"
     conclusion rests on, not a rendered screenshot.

### Verification

```
$ ls -la public/models/airwriter-case.glb
-rw-r--r--  1 wells  staff  187664 ... public/models/airwriter-case.glb
$ node -e "...magic/bytes check..."
magic glTF bytes 187664
```

- Magic bytes: `glTF` — confirmed.
- Size: 187,664 bytes (~183 KiB), well under 2MB. (1.28MB before the
  dedup/prune/weld/meshopt pass, ~85% smaller after.)
- Extents/orientation, via `scripts/board-model/tools/nodeinfo.mjs
  public/models/airwriter-case.glb case`:
  `translation (mm) 0.000, 11.470, 0.000`, `scale 0.055135...`, local bounds
  x [-616.66, 616.66], y [-208.01, 208.01], z [-1000, 1000] -> world size
  **68.00 x 22.94 x 110.27mm** (x, y, z) in metres in the file (x0.001 scale),
  matching the tray's real dimensions exactly (68.00 x 110.27 x 22.94mm, with
  height now on Y). Also cross-checked with
  `scripts/board-model/tools/bounds.mjs` (world-space per top-level node):
  size 0.07, 0.02, 0.11 (rounded to 2 places — consistent, just lower
  precision than `nodeinfo.mjs`'s local-bounds-times-scale figure above).

## Files changed

**airwriter repo** (branch `wells/airwriter-enclosure`, commit `c630b59`):
- `enclosure/test_case.py` — new corner-fillet-geometry test.
- `enclosure/README.md` — new "Shop model" section.

**shop repo** (branch `main`, commit `9759c69`):
- `public/models/airwriter-case.glb` — new file (188KB).

Not committed / not touched:
- `hardware/airwriter/airwriter.kicad_pro`, `hardware/combined/combined.kicad_pro`
  — pre-existing working-tree changes in the airwriter repo, unrelated to this
  task; `hardware/` was never edited by me, per the constraint.
- `firmware/.cache/`, `firmware/build_c6/`, `firmware/sdkconfig.c6` —
  pre-existing untracked build artifacts in the airwriter repo, untouched.
- `src/components/product/BuyPanel.astro`, `src/components/product/BuySummary.astro`,
  `src/lib/board-viewer.ts`, `src/lib/stock.ts`, `test/stock-pills.test.ts` —
  pre-existing uncommitted changes in the shop repo (from a prior session's
  work), unrelated to this task; left exactly as found, not staged or
  committed.
- `scripts/addon-models/node_modules/`, `scripts/addon-models/package-lock.json`
  — `npm i` ran to make the converter and gltf-transform CLI runnable;
  `node_modules/` is gitignored (`.gitignore` line 7), so nothing new to
  commit there. `package-lock.json` was already tracked before this session
  (not a new file) and I did not change `package.json`'s dependency versions,
  so it's unmodified.
- `enclosure/out/*.{step,stl}` — gitignored build products, not committed, per
  the constraint.

## Self-review

- The new test targets exactly what the review flagged: the four *rounded
  corners*, via real cylinder geometry rather than another volume probe with
  margin. It would have caught the Task-10-era bug this project's own docs
  describe (square corners) just as loudly as it caught my synthetic break,
  since a square-cornered `shell()` has no cylindrical fillet faces at all —
  `filter_by(GeomType.CYLINDER)` would return fewer than 8 faces and the count
  assertion would fail immediately.
- I did not weaken the assertion to pass a red-then-restore cycle: the numbers
  in the report are the actual computed values from a real run, not
  hand-picked to look plausible.
- For the GLB: I verified dimensions and orientation from the file's own
  transform/bounds data (two independent tools: `nodeinfo.mjs` and
  `bounds.mjs`), not just "a file appeared" and not just trusting the
  converter's stdout log line. I also independently verified the "which end
  is up" question against the pre-conversion CAD geometry (the slice-probe),
  so the orientation conclusion doesn't rest solely on reading `orient()`'s
  four lines of arithmetic correctly.
- I did not modify `stl2glb.mjs`, `join-prims.mjs`, or any other shop tooling.
  The only shop-repo file changes are the new GLB itself and (gitignored,
  uncommitted) `node_modules/` from `npm i`.

## Concerns

- `scripts/board-model/tools/` has no `package.json` and isn't runnable
  standalone in a fresh checkout — `build.sh`'s own `optimize()` function
  (`cd "$here/../board-model/tools" && node join-prims.mjs ...`) would hit the
  same `ERR_MODULE_NOT_FOUND` I hit, unless there's some other setup (a global
  install, a different Node resolution mode, or a `node_modules` that exists
  in the maintainer's checkout but isn't tracked in git) that I'm missing. I
  did not touch this — flagging it as a possible pre-existing rough edge in
  the shop tooling, not something I introduced or fixed.
- The shop repo already has five files with unrelated uncommitted changes from
  before this task started (product/checkout components, `board-viewer.ts`,
  a stock test). I left them alone and did not commit them, but they mean
  `git status` in the shop repo is not clean even after my commit — worth the
  user's own attention separately.
- No visual/rendered confirmation (e.g. a headless GLB screenshot) of "the
  case looks right" beyond the geometric bounds/orientation checks above —
  I don't have a renderer for this in the current environment. The numeric
  checks are strong (footprint, height, and up-axis all match, plus the
  independent CAD-side slice probe), but a human glance at it in the actual
  product viewer once it's wired up would still be worthwhile.
