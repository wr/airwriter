# Working KiCad 10 with konnect, kicad-cli, and scripted pcbnew

A general playbook for an agent driving a KiCad 10 project through the `konnect`
MCP server, the `kicad-cli` binary, and the `pcbnew` Python module. It describes
the roles each tool plays well and the habits that keep an automated workflow
reliable and reproducible.

## The mental model — three tools, three jobs

- **Scripted `pcbnew`** is your editor. Programmatic mutations to the
  `.kicad_pcb` — placement, outlines, zones, footprint injection, routing,
  stitching, connectivity repair — belong in small, re-runnable Python scripts.
  They are deterministic and diff cleanly, so the board's history reads as a
  sequence of reviewable changes.
- **`kicad-cli`** is your verifier and exporter. Use it for ERC, DRC, and every
  fab output (gerbers, drill, position, BOM, renders). It reports and emits; keep
  editing in the scripts.
- **`konnect` MCP** is your interactive surface. Reach for it to open and save
  projects, browse the schematic, place or query parts, and drive the viewer.

Keeping edits in scripts, verification in `kicad-cli`, and interaction in konnect
gives you a workflow where every change is reproducible and every claim of
"clean" is backed by a command you can re-run.

## Locate the tools first

- **`kicad-cli` ships inside the KiCad application bundle**, so invoke it by its
  full path rather than assuming it is on `PATH`. On macOS that is
  `…/KiCad.app/Contents/MacOS/kicad-cli`. Confirm the version once at the start of
  a session so you know which flag set you are targeting.
- **Run `pcbnew` scripts with the Python interpreter bundled inside KiCad**, which
  is the one that has the `pcbnew` module available. Point your shebang or command
  at that interpreter explicitly.
- **Allowlist both** the `kicad-cli` path and the konnect MCP in your settings so
  routine calls run without interruption.

## kicad-cli — verify and export

**Check each subcommand's help before you use it.** KiCad 10's CLI flags differ
from earlier releases, so `kicad-cli <subcommand> --help` is the fastest way to
target the current syntax with confidence.

**ERC and DRC — the gate:**
```
kicad-cli sch erc  board.kicad_sch
kicad-cli pcb drc  --format json -o drc.json  board.kicad_pcb
kicad-cli pcb drc  --refill-zones --schematic-parity --severity-all  board.kicad_pcb
```
- **Add `--refill-zones`** so DRC evaluates freshly poured copper. This matters
  most right after routing, when zones may not reflect the latest tracks.
- **Add `--severity-all`** to have every rule enforced at error level, so
  lower-severity findings surface while you can still act on them.
- **Add `--schematic-parity`** to confirm the board and schematic agree on
  footprints and nets.
- **Read the whole `unconnected_items` list and treat each entry as real.** For
  ground integrity, the reliable check is to compute the connected components of
  the ground copper (zone fills + pads + tracks + vias, with vias bridging layers)
  and confirm every ground pad lands in the main-plane component.

**Fab export:**
```
kicad-cli pcb export gerbers -o fab/  board.kicad_pcb
kicad-cli pcb export drill   --excellon-separate-th --generate-map --map-format gerberx2 -o fab/  board.kicad_pcb
kicad-cli pcb export pos     --format csv --units mm --exclude-dnp -o fab/pos.csv  board.kicad_pcb
kicad-cli sch export bom     --fields "Reference,Value,Footprint,LCSC,${QUANTITY},Note" \
  --labels "Designator,Comment,Footprint,LCSC,Qty,Note" \
  --group-by "Value,Footprint,LCSC,DNP" --exclude-dnp  -o fab/bom.csv  board.kicad_sch
```
- **Drive the BOM from the schematic** with `sch export bom`, and **group by `DNP`
  alongside value and footprint** so populated and do-not-populate parts stay in
  separate rows.
- **Pass `--exclude-dnp`** on the position file and BOM so the assembler sees only
  parts that should be placed.
- **After exporting drills, confirm the drill table** and keep every hole at or
  above your fab's standard minimum diameter — this keeps the board in the
  standard (lower-cost) process tier:
  ```
  kicad-cli pcb export drill --format excellon -o fab/  board.kicad_pcb
  grep -E '^T[0-9]+C' fab/board.drl
  ```

## Scripted pcbnew — programmatic edits and full routing automation

The `pcbnew` module gives you complete control over the board and, importantly,
still exposes `ExportSpecctraDSN` and `ImportSpecctraSES` even though the KiCad 10
CLI does not. That means the entire route round-trip can run headless from a
script. General habits that keep scripted edits trustworthy:

- **Run each script with KiCad's bundled interpreter**, and keep scripts small and
  single-purpose so they compose and re-run cleanly.
- **Make on-disk edits with the KiCad GUI closed, then open KiCad to review.**
  This keeps the file your script wrote authoritative and your review honest.
- **Set UUIDs explicitly** when you need them stable across saves, so regenerated
  files stay diff-friendly.
- **When you move a footprint, move its keepout and zone polygons with it** —
  those polygons carry absolute coordinates, so shifting them together keeps
  rule areas aligned to the part.
- **Apply multi-step board mutations as a sequence of separate script runs,
  saving between them.** A clean save-and-reload between phases keeps later
  lookups (footprints, nets) consistent.
- **Persist zone fills by running the zone filler inside a script**, then save —
  that writes the poured copper to disk (a DRC-time refill is in-memory only).
- **Verify save-sensitive geometry by vertex count or a render** rather than by a
  single reported count, so you confirm what actually landed on disk.

## Freerouting round-trip

A script-driven route generally follows: export DSN → normalize → route → import
SES → refill → DRC. To make it dependable:

- **Round the coordinates in the exported `.dsn` to whole units** before handing
  it to Freerouting. KiCad emits fractional coordinates; integer coordinates keep
  the router fast and stable.
- **Route from a clean slate each pass** — export, route, import fresh — for
  predictable, repeatable results.
- **Run the router as a detached process** so it survives your tool shell being
  reset mid-run.
- **Refill zones and re-run DRC afterward** (`--refill-zones`), since the router
  produces tracks but not poured copper.

## konnect MCP — interactive placement, viewing, and project I/O

konnect shines for the interactive parts of the loop: opening and saving the
project, browsing and rendering the schematic, and placing or querying parts.

- **Make custom libraries discoverable to konnect** by placing them where KiCad's
  install resolves libraries (symlinking a project library into KiCad's install
  symbol/footprint directories works well). Re-establish those links after a
  KiCad upgrade.
- **Edit DRC constraints directly in the `.kicad_pro` file**, which is where
  KiCad 10 stores them — that keeps the rule set in one authoritative place.
- **Build and modify the board outline in a pcbnew script**, where you have full
  control over replacing the existing Edge.Cuts geometry.

## Ground and net hygiene

- **Create ground (and other) zones on the exact hierarchical net name your pads
  use** — e.g. `/GND` with its leading slash — and **verify the assignment by the
  net string**, so the pour bonds to the intended net.
- **Keep the model reference with the footprint on the board** when a part carries
  a custom 3D model, so STEP/STL exports render every part.

## The verification battery

Run this before treating a package as fab-ready, and record the expected counts so
a future run can spot drift:
```
kicad-cli sch erc  board.kicad_sch                                   # 0 errors
kicad-cli pcb drc  --refill-zones --schematic-parity --severity-all  board.kicad_pcb
grep -E '^T[0-9]+C' fab/*.drl                                        # every drill at/above the fab floor
# position-file row count == expected placement count
```
Pair the battery with a short note of the expected results (row counts, any
known-benign findings) so "clean" always means "matches the recorded baseline."
