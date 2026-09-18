### Task 5: Port cutouts booleaned from the real connector STEPs

**Files:**
- Modify: `enclosure/case.py`
- Modify: `enclosure/test_case.py`

**Interfaces:**
- Consumes: `board_geom.footprint()`, the `.step` files under `hardware/airwriter/lib/airwriter.3dshapes/`
- Produces: `connector_solid(ref: str) -> Part` — the connector's own geometry, placed in the case frame; `port_cutouts() -> Part` — the offset solids to subtract from the front wall

This is the reason the toolchain is OCCT and not OpenSCAD. J2, J4 and J1 all carry real vendor STEP files, so no barrel diameter, flange size or shell height is ever guessed — the wall opening is the connector's own solid, grown by `PORT_CLEAR`.

- [ ] **Step 1: Write the failing test**

Append to `enclosure/test_case.py`, above the `if __name__` block:

```python
class TestPorts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tray_shell = case.shell() - case.port_cutouts()

    def test_each_connector_has_real_step_geometry(self):
        for ref in ("J2", "J4", "J1"):
            solid = case.connector_solid(ref)
            self.assertTrue(solid.is_valid, ref)
            self.assertGreater(solid.volume, 100.0, ref)

    def test_minidin_mating_faces_stand_2_75_proud_of_the_board_edge(self):
        # Y=0 is the board's front edge; the footprint origin IS the mating
        # face, so the connector reaches Y = -2.75.
        for ref in ("J2", "J4"):
            bb = case.connector_solid(ref).bounding_box()
            self.assertAlmostEqual(bb.min.Y, -2.75, places=2, msg=ref)

    def test_no_connector_touches_the_tray(self):
        for ref in ("J2", "J4", "J1"):
            hit = self.tray_shell & case.connector_solid(ref)
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_openings_clear_each_connector_by_at_least_0_30(self):
        from build123d import offset
        for ref in ("J2", "J4", "J1"):
            grown = offset(case.connector_solid(ref), 0.30)
            hit = self.tray_shell & grown
            self.assertAlmostEqual(hit.volume, 0.0, places=3, msg=ref)

    def test_front_wall_still_exists_between_the_two_minidins(self):
        # J2 spans X 4.2..21.8 and J4 23.7..41.3; there must be plastic between.
        from build123d import Box, Pos
        probe = Pos(22.75, -p.WALL / 2, 5.0) * Box(1.0, p.WALL, 2.0)
        self.assertGreater((self.tray_shell & probe).volume, 0.5)
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: FAIL — `AttributeError: module 'case' has no attribute 'port_cutouts'`

- [ ] **Step 3: Implement**

Add to `enclosure/case.py` — extend the import line to
`from build123d import Box, Cone, Cylinder, Part, Pos, Rot, import_step, offset` and add:

```python
def connector_solid(ref: str) -> Part:
    """A connector's own vendor STEP, placed in the case frame.

    KiCad stores a footprint's model offset/rotation/scale, but every model
    this project touches is placed at offset 0, scale 1 with the part's own
    origin on the footprint origin, so the placement here is the footprint's
    position and rotation and nothing else. If a model is ever given a
    non-zero offset in the board file, this function has to grow to read it.
    """
    fp = bg.footprint(ref)
    if not fp.model or not fp.model.endswith(".step"):
        raise RuntimeError(f"{ref} has no STEP model: {fp.model}")
    part = import_step(fp.model)

    # STEP models are authored in the footprint's own frame: +X right,
    # +Y "into the board" (KiCad's y-down), Z up off the board. The case frame
    # flips Y, so the model turns by (180 - rot) about Z.
    x, y = bg.to_case(fp.x, fp.y)
    return Pos(x, y, 0) * Rot(0, 0, 180.0 - fp.rot) * part


def port_cutouts() -> Part:
    """What the front wall gives up to J2, J4 and J1.

    Each opening is the connector's own solid grown by PORT_CLEAR, so the
    barrel diameters, flanges and shell heights come from the vendor geometry
    rather than from a datasheet reading. The USB-C receptacle sits ~1.5mm
    behind the wall's outer face, so its opening also gets an outside chamfer
    wide enough for a plug's overmould.
    """
    out = None
    for ref in ("J2", "J4", "J1"):
        cut = offset(connector_solid(ref), p.PORT_CLEAR)
        out = cut if out is None else out + cut

    j1 = connector_solid("J1").bounding_box()
    flare = p.PORT_CLEAR + p.USBC_CHAMFER
    chamfer = Pos(
        (j1.min.X + j1.max.X) / 2,
        -p.WALL + p.USBC_CHAMFER / 2,
        (j1.min.Z + j1.max.Z) / 2,
    ) * Cone(
        (j1.size.X + 2 * flare) / 2,
        (j1.size.X + 2 * p.PORT_CLEAR) / 2,
        p.USBC_CHAMFER,
        rotation=(90, 0, 0),
    )
    return out + chamfer
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd ~/Projects/airwriter/enclosure && .venv/bin/python -m unittest -v test_case`
Expected: PASS, 13 tests.

If `test_minidin_mating_faces_stand_2_75_proud_of_the_board_edge` fails with the sign of Y reversed, the `180.0 - fp.rot` in `connector_solid` is wrong for this board's rotation convention — try `-fp.rot` and re-read the comment in `board_geom.local_to_board`. Do not "fix" it by editing the expected value; 2.75 proud is measured from the board file.

- [ ] **Step 5: Commit**

```bash
cd ~/Projects/airwriter
git add enclosure/case.py enclosure/test_case.py
git commit -m "Enclosure: port openings cut from the real connector STEPs"
```

---

