### Task 9: The shop's GLB

**Files:**
- Modify: `enclosure/README.md`
- Create (in the **shop** repo): `~/Projects/shop/public/models/airwriter-case.glb`

**Interfaces:**
- Consumes: `enclosure/out/airwriter-tray.stl`
- Produces: a GLB the shop's product page can use. Wiring it to a product is explicitly out of scope (spec §8).

- [ ] **Step 1: Convert the tray**

Run:

```bash
cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
  ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
  public/models/airwriter-case.glb
```

If `stl2glb.mjs` takes different arguments, read its header comment and adjust; do not rewrite the converter.

- [ ] **Step 2: Check the result loads and is a sane size**

Run:

```bash
cd ~/Projects/shop && ls -la public/models/airwriter-case.glb && \
  node -e "const b=require('fs').readFileSync('public/models/airwriter-case.glb');console.log('magic',b.toString('utf8',0,4),'bytes',b.length)"
```

Expected: `magic glTF` and a file well under 2 MB.

- [ ] **Step 3: Record the recipe**

Append to `enclosure/README.md`:

```markdown
## Shop model

    cd ~/Projects/shop && node scripts/addon-models/stl2glb.mjs \
      ~/Projects/airwriter/enclosure/out/airwriter-tray.stl \
      public/models/airwriter-case.glb

Nothing on the site references it yet — making the case a purchasable add-on is
a product decision, not part of this design.
```

- [ ] **Step 4: Commit, in both repos**

```bash
cd ~/Projects/airwriter && git add enclosure/README.md && \
  git commit -m "Enclosure: record the shop GLB recipe"
cd ~/Projects/shop && git add public/models/airwriter-case.glb && \
  git commit -m "AirWriter case model"
```

---

## Self-review notes

- **Spec coverage.** §1 parts → Tasks 3–7. §2 stack → Task 2 (`test_z_landmarks`). §3 uniform wall → Task 2 + `test_walls_are_at_least_2_80_thick`. §4 front wall → Task 5; west/east/carve-outs → Task 6; top seat → Task 3 + Task 7. §5 fastening → Task 4 (Pro) and Task 7 (LC inserts). §6 source layout → Tasks 1, 2, 8. §7 verification → Task 8's gate plus the README's first-print order; the KiCad interference check is manual and the README says so. §8 shop GLB → Task 9. §9 out-of-scope is honoured: no task touches `hardware/`.
- **Open items carried, not resolved.** Insert MPN (`INSERT_D`/`INSERT_DEPTH` are re-cuttable constants), rubber foot, USB-C chamfer (`USBC_CHAMFER`, tuned on the first print). Each is a named constant rather than a buried literal.
- **The two soft numbers**, both flagged at their use site: `DIP_SLOT_Z0/Z1` (from HANDOFF's 8.11–9.01 tip travel, no STEP to boolean against) and `reset_pinhole()`'s Z (SW1 has only a `.wrl`). Everything else is either read from the board or booleaned from vendor geometry.
