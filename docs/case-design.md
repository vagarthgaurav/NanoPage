# NanoPage 3D-printed case: design spec

Implemented by `Case/nanopage_case.py` (FreeCAD 0.21.2, run headless with `freecadcmd`).
All numbers were measured from the KiCad PCB, the full-board STEP export and the
datasheets (display GDEY029T94, battery EEMB LP402535). Nothing here is from project notes.

## Coordinate frame

Use the KiCad STEP-export frame so the case can be checked against the board directly:

- `X = kicad_x`, `Y = -kicad_y`, `Z` up, PCB bottom face at `Z = 0`, PCB top face at `Z = 1.57`.
- Components are on the front (top) side, `Z > 1.57`. The display is on the back, `Z < 0`.
- "Front" of the device = the screen side = `-Z`. "Rear" = `+Z`.
- Quoted below as `kicad (x, y)` where a KiCad coordinate is clearer. Convert with `Y = -y`.

Regenerate the reference STEP when the board changes:
`kicad-cli pcb export step --subst-models --no-dnp -o Case/ref/nanopage_full.step Hardware/NanoPage/NanoPage.kicad_pcb`
(`FL1`'s 3D model is missing; that is expected and harmless.)

## Inputs (measured)

- PCB: x 101.4..193.2, kicad y 85.5..123.0 (91.8 x 37.5), 1.57 thick, corner radius 1.5.
  Right-end notch (kicad coords): (193.2,93.5) -> (189.7,96.75) -> (189.7,111.75) -> (193.2,115).
  Take the exact outline from the `z=0` face of the board solid in the STEP, don't retype it.
- Mounting holes (M2, 2.2 dia, countersunk DIN965 intended): kicad (103.9,88), (190.7,88), (103.9,120.5), (190.7,120.5).
- Display glass 79.0 x 36.7 x 1.2, centre kicad (147.35, 104.25) (from the two back-side silkscreen corner marks).
  Glass x 107.85..186.85, kicad y 85.9..122.6. Glued to the PCB back with 0.15 tape: glass z -1.35..-0.15.
  FPC exits the right end (x=186.85), 12.5 wide, kicad y ~98..110.5, wraps around the notch edge at x=189.7
  and lands on J3 (x 177.9..183.6, z up to 3.76) on the front side.
- Active area 66.9 x 29.06, centre kicad (144.3, 104.25). Window = 68.0 x 30.0 centred there
  (x 110.3..178.3, kicad y 89.25..119.25).
- Battery EEMB LP402535 (box 25.5 x 37.0 x 4.3, plus 0.4 swell allowance): 25.5 along X, 37.0 along kicad Y.
  Centre kicad (166.0, 104.25); bottom at Z=5.0 (clears the tallest nearby part, J5 top = 4.5), top at Z=9.3.
- Heights (STEP, absolute Z): SWD header J4 5.90 (x 131.6..136.6, kicad y 89.3..92.2); tact switches S1-S3 4.94;
  USB-C 4.86 (front face at kicad y 85.04, opening body Z 1.70..4.86, centre Z 3.28, x 142.83..151.77);
  E73 4.66; battery connector J5 4.50 (x 166.4..170.7, kicad y 90.2..94.2); J3 3.76; microSD 2.97
  (x 141.3..153.3, kicad y 107.5..118.9, flush with the bottom board edge, slot Z 1.55..2.97).
- Side buttons S1-S3 (side-actuated): kicad x 111.4, 123.4, 183.2. Round actuator dia ~2.2, axis Z = 3.28,
  tip at kicad y = 85.95 (0.45 inside the board edge). Actuators face the top edge (decreasing kicad y).
- Reset button S4: top-actuated, kicad (132.1, 102.5), body x 129.45..134.75, kicad y 99.85..105.15.
  Query the STEP for its actual top Z.

## Parts

### 1. Front shell (bezel + walls), print bezel face down

- Interior: x 101.0..193.8, kicad y 84.6..123.4, Z from -1.45 (bezel plate back face) to 9.9.
  Interior vertical corner radius 1.0.
- Wall 2.0 thick: outer x 99.0..195.8, kicad y 82.6..125.4, outer vertical corner radius 3.0.
  Outside edge fillet 0.8 on the front face edge.
- Bezel plate: 1.2 thick, Z -2.65..-1.45, covering the full outer outline.
- Display window through the plate: 68.0 x 30.0 (see above), corner radius 1.0, 0.6 x 45-degree chamfer on the front face edge.
- Glass guide ribs on the plate's back: four L-shaped corner stops around the glass outline offset 0.3 outward,
  0.8 thick, 8 mm legs, Z -1.45..-0.15. None in the FPC corridor (see keep-outs).
- Cover rebate: the inner 1.0 of the wall (inner skin) stops at Z = 9.9; the outer 1.0 skin continues to Z = 11.1.
  So the rear cover sits in a recess with its top flush with the walls.
- Four screw posts at the mounting-hole centres: OD 7.0, Z -1.45..0.0 (top face touches the PCB back).
  Each has a dia 3.2 blind pocket for an M2 heat-set insert, cut down from Z=0 to Z=-2.0 (0.65 floor above the front face).
  Insert goes in from the PCB side with a soldering iron, flush with the post top. Insert must be <= 2.0 mm long.
  Posts merge with the interior walls where they overlap. Keep the 0.45 and 0.35 clearances to the glass edges.
- Wall openings (all through the wall only; Z values are absolute):
  - Top wall (kicad y 82.6..84.6): USB-C centre x 147.3, Z 3.28, 11.0 W x 5.0 H, corner radius 2.0.
    No SWD opening (J4 is not reachable once the case is closed).
    Three obround button holes 6.4 W (X) x 3.4 H (Z), fully rounded short sides, at x 111.4, 123.4, 183.2, Z 3.28.
  - Bottom wall (kicad y 123.4..125.4): microSD centre x 147.3, Z 2.5, 12.6 W x 2.6 H, corner radius 0.8.
  - Not provided: J6 (external-button FPC). Leave a clearly named parameter to add it later.

### 2. Rear cover

- Plate 1.2 thick, Z 9.9..11.1, outline = rebate outline (interior offset +1.0) shrunk by 0.15 clearance.
- Four screw tubes at the mounting-hole centres: OD 4.2, ID 2.4, Z 1.57..9.9 (bottom face rests on the PCB top).
  Countersink for M2 DIN965 flat head (dia 4.0 at the top face) through the plate.
  Screws: M2 x 12 countersunk, tip lands in the heat-set insert (tip at Z -0.9, so ~0.9 mm of thread engagement).
- Reset pin guide: tube OD 4.0, ID 1.8 at kicad (132.1, 102.5), from Z 9.9 down to (S4 top + 0.5), through-hole dia 1.8 in the plate.
- Battery guide ribs: four ribs 1.0 thick x 8 long, at x = 151.95..152.95 and 179.05..180.05 (battery x edges +/-0.3),
  kicad y 92..100 and 108..116, Z 6.0..9.9.
- Optional text: "NanoPage" 1.0 deep engraved, only if it is a few lines of code. Skip otherwise.

### 3. Button cap (print x3)

- Obround stem 6.0 W (X) x 3.0 H (Z) from kicad y = 82.0 to 85.3; obround flange 8.0 x 5.2 x 0.7 thick (y 84.6..85.3) against the wall inner face,
  clipped flat at Z >= 1.9 so it can't touch the PCB edge (PCB edge is kicad y = 85.5, top at Z=1.57).
- Pin dia 2.0 from y 85.3 to 85.8 pointing at the actuator tip (tip at 85.95).
- The switch spring holds the cap against the wall. Caps are inserted from the inside before the PCB goes in.

## Keep-outs (must stay empty of printed geometry)

- FPC corridor: x 186.8..194.5, kicad y 96.6..112.4, Z -1.6..4.5.
- Battery box (25.5 x 37.0 x 4.3 at the position above, plus 0.4 swell on top).
- Display glass box.
- The whole PCB assembly from the STEP, except the mounting-hole contact faces (posts touch PCB back, tubes touch PCB top).
- The antenna zone on the E73 (left end, x 101..121) has no metal. Printed plastic only.

## Deliverables

All in `Case/` (create it); outputs in `Case/out/` and reference data in `Case/ref/`.

- `Case/nanopage_case.py`: parameters in one block at the top, geometry as functions, one `main()`.
  Run: `freecadcmd Case/nanopage_case.py`. Must run headless and be idempotent. Beware: don't name
  any script `profile.py` (it shadows a stdlib module in freecadcmd).
- `Case/out/front_shell.step|stl`, `rear_cover.step|stl`, `button_cap.step|stl`.
- `Case/out/NanoPage_case.FCStd`: all parts plus ghost bodies (PCB from the STEP, display glass, battery box)
  so the user can open it in the FreeCAD GUI.
- A fit check printed at the end: for each printed part, the intersection volume with each keep-out, in mm^3.
  Every number must be 0.00 (tolerance 0.01), except the intended contacts above. Print PASS/FAIL per check.
  Also print the overall outer size (expected about 96.8 x 42.8 x 13.75).

## Known limits

- The battery is boxed; its actual lead/tab position is unknown, so it is a free parameter.
- The case is about 13.75 mm thick, mostly because the battery has to ride 3.4 mm above the PCB to clear J5.
  A lower J5 or one moved off the battery area would save about 2 mm.
- Wall openings are sized for a typical USB-C overmold and a 3-pin JST-SH plug, not measured against the user's cables.
