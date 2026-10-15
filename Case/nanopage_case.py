# NanoPage 3D-printed case, parametric FreeCAD 0.21.2 script.
# Spec: docs/case-design.md.   Run:  freecadcmd Case/nanopage_case.py
#
# Frame (KiCad STEP-export frame): X = kicad_x, Y = -kicad_y, Z up,
# PCB bottom at Z = 0, PCB top at Z = 1.57. Screen (front) = -Z, rear = +Z.
# All "ky" values below are KiCad Y coordinates; helpers convert to Y = -ky.

import os
import sys
import math
import zipfile
import shutil
import FreeCAD as App
import Part
from FreeCAD import Vector as V

# ----------------------------------------------------------------------------
# PARAMETERS (all dimensions in mm)
# ----------------------------------------------------------------------------
try:
    CASE_DIR = os.path.dirname(os.path.abspath(__file__))
except NameError:  # freecadcmd may exec the script without __file__
    CASE_DIR = os.path.join(os.getcwd(), "Case")
REPO_DIR = os.path.dirname(CASE_DIR)
STEP_REF = os.path.join(CASE_DIR, "ref", "nanopage_full.step")
OUT_DIR = os.path.join(CASE_DIR, "out")
S4_MODEL_FILE = os.path.join(REPO_DIR, "Hardware", "NanoPage", "libraries",
                             "NanoPage.3dshapes", "PTS526SM15SMTR2.step")

# --- board / frame
PCB_T = 1.57
PCB_TOP = PCB_T
HOLES_KY = [(103.9, 88.0), (190.7, 88.0), (103.9, 120.5), (190.7, 120.5)]  # (x, ky)

# --- display glass (back of the PCB)
GLASS_X = (107.85, 186.85)
GLASS_KY = (85.9, 122.6)
GLASS_Z = (-1.35, -0.15)
WINDOW_C = (144.3, 104.25)          # active-area centre (x, ky)
WINDOW_W, WINDOW_H = 68.0, 30.0
WINDOW_R = 1.0
WINDOW_CHAMFER = 0.6                # 45 degrees, front face edge

# --- battery (box + swell allowance on top)
BAT_C = (166.0, 104.25)             # centre (x, ky)
BAT_W, BAT_L, BAT_T = 25.5, 37.0, 4.3   # X, ky, Z
BAT_Z0 = 5.0
BAT_SWELL = 0.4

# --- front shell
INT_X = (101.0, 193.8)
INT_KY = (84.6, 123.4)
INT_R = 1.0
WALL = 2.0
OUT_R = 3.0
PLATE_T = 1.2
PLATE_Z = (-2.65, -1.45)
INT_Z_TOP = 9.9                     # inner skin top / rear cover plate bottom
SHELL_Z_TOP = 11.1                  # outer skin top / rear cover top
REBATE = 1.0                        # inner skin thickness (outer skin = WALL-REBATE)
FRONT_FILLET = 0.8
RIB_OFFSET, RIB_T, RIB_LEG = 0.3, 0.8, 8.0   # glass guide corner stops
RIB_Z = (-1.45, -0.15)
POST_OD = 7.0
POST_Z = (-1.45, 0.0)
INSERT_HOLE_D = 3.2                 # M2 heat-set insert pocket, blind, melted in from the PCB side
INSERT_Z0 = -2.0                    # pocket floor; leaves 0.65 above the front face (PLATE_Z[0])

# wall openings: (centre x, centre z, width, height, corner radius)
OPEN_USB = (147.3, 3.28, 11.0, 5.0, 2.0)      # top wall
OPEN_SD = (147.3, 2.5, 12.6, 2.6, 0.8)        # bottom wall
BUTTON_X = [111.4, 123.4, 183.2]
BUTTON_Z = 3.28
BUTTON_HOLE_W, BUTTON_HOLE_H = 6.4, 3.4       # obround (stadium) hole, long axis along X
# J6 (external-button FPC): not provided. Add (centre x, centre z, w, h, r) here later.
OPEN_J6 = None

# --- rear cover
COVER_CLEAR = 0.15
TUBE_OD, TUBE_ID = 4.2, 2.4
TUBE_Z0 = PCB_TOP
CSK_D = 4.0                         # M2 DIN965 head dia at top face (90 deg countersink)
RESET_XY_KY = (132.1, 102.5)
RESET_OD, RESET_ID = 4.0, 1.8
RESET_BELOW_TOP = 0.5               # guide ends this far above S4 top
S4_TOP_FALLBACK = 4.0               # used only if neither STEP nor model give S4 top
S4_USE_MODEL_FILE = True            # False -> go straight to S4_TOP_FALLBACK when absent in STEP
S4_BODY_X = (129.45, 134.75)
S4_BODY_KY = (99.85, 105.15)
BAT_RIB_X = [(151.95, 152.95), (179.05, 180.05)]
BAT_RIB_KY = [(92.0, 100.0), (108.0, 116.0)]
BAT_RIB_Z = (6.0, 9.9)

# --- button cap
CAP_STEM_W, CAP_STEM_H = 6.0, 3.0   # obround, 0.2 clearance per side in the hole
CAP_STEM_KY = (82.0, 85.3)
CAP_FLANGE_W, CAP_FLANGE_H, CAP_FLANGE_KY = 8.0, 5.2, (84.6, 85.3)
CAP_FLANGE_ZMIN = 1.9
CAP_PIN_D, CAP_PIN_KY = 2.0, (85.3, 85.8)

# --- keep-outs
FPC_X = (186.8, 194.5)
FPC_KY = (96.6, 112.4)
FPC_Z = (-1.6, 4.5)
TOL = 0.01


# ----------------------------------------------------------------------------
# HELPERS
# ----------------------------------------------------------------------------
def yr(ky0, ky1):
    """KiCad y range -> (Ymin, Ymax) in STEP frame."""
    return (-ky1, -ky0)


def rr_wire(x0, x1, y0, y1, z, r):
    """Rounded-rectangle wire in the plane Z=z (STEP-frame X/Y, CCW)."""
    k = r * (1 - math.sqrt(0.5))   # mid-arc offset from the corner
    p = V
    e = [
        Part.LineSegment(p(x0 + r, y0, z), p(x1 - r, y0, z)).toShape(),
        Part.Arc(p(x1 - r, y0, z), p(x1 - k, y0 + k, z), p(x1, y0 + r, z)).toShape(),
        Part.LineSegment(p(x1, y0 + r, z), p(x1, y1 - r, z)).toShape(),
        Part.Arc(p(x1, y1 - r, z), p(x1 - k, y1 - k, z), p(x1 - r, y1, z)).toShape(),
        Part.LineSegment(p(x1 - r, y1, z), p(x0 + r, y1, z)).toShape(),
        Part.Arc(p(x0 + r, y1, z), p(x0 + k, y1 - k, z), p(x0, y1 - r, z)).toShape(),
        Part.LineSegment(p(x0, y1 - r, z), p(x0, y0 + r, z)).toShape(),
        Part.Arc(p(x0, y0 + r, z), p(x0 + k, y0 + k, z), p(x0 + r, y0, z)).toShape(),
    ]
    return Part.Wire(e)


def rrect(x0, x1, ky0, ky1, z0, z1, r):
    """Rounded-corner prism, vertical edges rounded with radius r (KiCad y range)."""
    ya, yb = yr(ky0, ky1)
    w = rr_wire(x0, x1, ya, yb, z0, r)
    return Part.Face(w).extrude(V(0, 0, z1 - z0))


def box(x0, x1, ky0, ky1, z0, z1):
    ya, yb = yr(ky0, ky1)
    return Part.makeBox(x1 - x0, yb - ya, z1 - z0, V(x0, ya, z0))


def cyl_z(x, ky, z0, z1, d):
    return Part.makeCylinder(d / 2.0, z1 - z0, V(x, -ky, z0), V(0, 0, 1))


def cyl_y(x, z, ky0, ky1, d):
    """Cylinder along the KiCad-y axis from ky0 to ky1 (ky1 > ky0)."""
    return Part.makeCylinder(d / 2.0, ky1 - ky0, V(x, -ky0, z), V(0, -1, 0))


def opening(spec, ky0, ky1):
    """Rounded-rectangle opening through a wall (axis along Y), absolute Z."""
    xc, zc, w, h, r = spec
    # outline in the XZ plane, extruded along Y; build as a box then fillet the Y-parallel edges
    ya, yb = yr(ky0, ky1)
    b = Part.makeBox(w, yb - ya, h, V(xc - w / 2.0, ya, zc - h / 2.0))
    edges = [e for e in b.Edges
             if abs(e.Vertexes[0].X - e.Vertexes[1].X) < 1e-9
             and abs(e.Vertexes[0].Z - e.Vertexes[1].Z) < 1e-9]
    return b.makeFillet(r, edges)


def pill_y(x, z, ky0, ky1, w, h):
    """Obround prism along the KiCad-y axis: w wide (X), h tall (Z), fully rounded short sides."""
    r = h / 2.0
    parts = [cyl_y(x - w / 2.0 + r, z, ky0, ky1, h), cyl_y(x + w / 2.0 - r, z, ky0, ky1, h)]
    ya, yb = yr(ky0, ky1)
    parts.append(Part.makeBox(w - h, yb - ya, h, V(x - w / 2.0 + r, ya, z - r)))
    return fuse_all(parts)


def as_solid(s):
    """Boolean results come back as Compounds; unwrap when there is exactly one solid."""
    return s.Solids[0] if len(s.Solids) == 1 else s


def fuse_all(shapes):
    s = shapes[0]
    for o in shapes[1:]:
        s = s.fuse(o)
    return as_solid(s.removeSplitter())


def cut_all(base, cutters):
    s = base
    for c in cutters:
        s = s.cut(c)
    return as_solid(s.removeSplitter())


def vol_common(a, b):
    if not a.BoundBox.intersect(b.BoundBox):
        return 0.0
    try:
        return a.common(b).Volume
    except Exception:
        return float("nan")


# ----------------------------------------------------------------------------
# REFERENCE DATA FROM THE KICAD STEP
# ----------------------------------------------------------------------------
def load_reference():
    ref = Part.read(STEP_REF)
    board = max(ref.Solids, key=lambda s: s.Volume)
    outline = None
    for f in board.Faces:
        bb = f.BoundBox
        if abs(bb.ZMin) < 1e-6 and abs(bb.ZMax) < 1e-6:
            if outline is None or f.Area > outline.Area:
                outline = f
    return ref, board, outline.OuterWire


def find_s4_top(ref):
    """Top Z of S4. Returns (z, source_text)."""
    best = None
    for so in ref.Solids:
        b = so.BoundBox
        if (b.XMin >= S4_BODY_X[0] - 0.3 and b.XMax <= S4_BODY_X[1] + 0.3 and
                -b.YMax >= S4_BODY_KY[0] - 0.3 and -b.YMin <= S4_BODY_KY[1] + 0.3 and
                b.ZMax > PCB_TOP and so.Volume > 0.3):
            best = b.ZMax if best is None else max(best, b.ZMax)
    if best is not None:
        return best, "STEP"
    # S4 is marked DNP in the PCB, so `--no-dnp` drops it from the STEP.
    # Next best: its own 3D model (footprint places it with a +0.05 Z offset).
    if S4_USE_MODEL_FILE and os.path.exists(S4_MODEL_FILE):
        m = Part.read(S4_MODEL_FILE)
        return PCB_TOP + 0.05 + m.BoundBox.ZMax, "S4 model file (S4 is DNP, absent from STEP)"
    return S4_TOP_FALLBACK, "fallback constant (S4 not in STEP)"


# ----------------------------------------------------------------------------
# PARTS
# ----------------------------------------------------------------------------
def make_front_shell():
    ox0, ox1 = INT_X[0] - WALL, INT_X[1] + WALL
    oky0, oky1 = INT_KY[0] - WALL, INT_KY[1] + WALL
    zf = PLATE_Z[0]

    outer = rrect(ox0, ox1, oky0, oky1, zf, SHELL_Z_TOP, OUT_R)
    front_edges = [e for e in outer.Edges
                   if abs(e.Vertexes[0].Z - zf) < 1e-6 and abs(e.Vertexes[-1].Z - zf) < 1e-6]
    outer = outer.makeFillet(FRONT_FILLET, front_edges)

    cavity = rrect(INT_X[0], INT_X[1], INT_KY[0], INT_KY[1], PLATE_Z[1], INT_Z_TOP + 0.001, INT_R)
    rebate = rrect(INT_X[0] - REBATE, INT_X[1] + REBATE, INT_KY[0] - REBATE, INT_KY[1] + REBATE,
                   INT_Z_TOP, SHELL_Z_TOP + 1.0, INT_R + REBATE)
    shell = outer.cut(cavity).cut(rebate)

    # display window with 45-degree chamfer on the front edge
    wx0, wx1 = WINDOW_C[0] - WINDOW_W / 2, WINDOW_C[0] + WINDOW_W / 2
    wk0, wk1 = WINDOW_C[1] - WINDOW_H / 2, WINDOW_C[1] + WINDOW_H / 2
    c = WINDOW_CHAMFER
    ya, yb = yr(wk0, wk1)
    w_front = rr_wire(wx0 - c, wx1 + c, ya - c, yb + c, zf - 0.001, WINDOW_R + c)
    w_mid = rr_wire(wx0, wx1, ya, yb, zf + c, WINDOW_R)
    cone = Part.makeLoft([w_front, w_mid], True, True)
    thru = rrect(wx0, wx1, wk0, wk1, zf + c - 0.001, PLATE_Z[1] + 0.001, WINDOW_R)
    shell = shell.cut(cone).cut(thru)

    adds = []
    # glass guide ribs: four L-shaped corner stops
    gx0, gx1 = GLASS_X
    gk0, gk1 = GLASS_KY
    o, t, L = RIB_OFFSET, RIB_T, RIB_LEG
    z0, z1 = RIB_Z
    for sx, sk in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        cx = gx0 if sx < 0 else gx1
        ck = gk0 if sk < 0 else gk1
        # outer corner of the L and the inner edge (toward the glass)
        xo, xi = cx + sx * (o + t), cx + sx * o
        ko, ki = ck + sk * (o + t), ck + sk * o
        xl = xo - sx * L
        kl = ko - sk * L
        leg_x = box(min(xo, xl), max(xo, xl), min(ko, ki), max(ko, ki), z0, z1)
        leg_k = box(min(xo, xi), max(xo, xi), min(ko, kl), max(ko, kl), z0, z1)
        adds += [leg_x, leg_k]
    # screw posts
    for (hx, hk) in HOLES_KY:
        adds.append(cyl_z(hx, hk, POST_Z[0], POST_Z[1], POST_OD))
    shell = fuse_all([shell] + adds)

    cuts = []
    for (hx, hk) in HOLES_KY:
        cuts.append(cyl_z(hx, hk, INSERT_Z0, POST_Z[1] + 0.001, INSERT_HOLE_D))
    # wall openings (through the wall only; stop at the inner wall faces)
    top_k = (INT_KY[0] - WALL - 1.0, INT_KY[0])
    bot_k = (INT_KY[1], INT_KY[1] + WALL + 1.0)
    cuts.append(opening(OPEN_USB, *top_k))
    for bx in BUTTON_X:
        cuts.append(pill_y(bx, BUTTON_Z, top_k[0], top_k[1], BUTTON_HOLE_W, BUTTON_HOLE_H))
    cuts.append(opening(OPEN_SD, *bot_k))
    if OPEN_J6 is not None:
        cuts.append(opening(OPEN_J6, *bot_k))
    shell = cut_all(shell, cuts)
    return shell


def rebate_outline_clear():
    return (INT_X[0] - REBATE + COVER_CLEAR, INT_X[1] + REBATE - COVER_CLEAR,
            INT_KY[0] - REBATE + COVER_CLEAR, INT_KY[1] + REBATE - COVER_CLEAR,
            INT_R + REBATE - COVER_CLEAR)


def make_rear_cover(s4_top):
    x0, x1, k0, k1, r = rebate_outline_clear()
    zt0, zt1 = INT_Z_TOP, SHELL_Z_TOP
    plate = rrect(x0, x1, k0, k1, zt0, zt1, r)
    adds = [plate]
    for (hx, hk) in HOLES_KY:
        adds.append(cyl_z(hx, hk, TUBE_Z0, zt0 + 0.001, TUBE_OD))
    rx, rk = RESET_XY_KY
    guide_z0 = s4_top + RESET_BELOW_TOP
    adds.append(cyl_z(rx, rk, guide_z0, zt0 + 0.001, RESET_OD))
    for bx in BAT_RIB_X:
        for bk in BAT_RIB_KY:
            adds.append(box(bx[0], bx[1], bk[0], bk[1], BAT_RIB_Z[0], BAT_RIB_Z[1] + 0.001))
    cover = fuse_all(adds)

    cuts = []
    depth = (CSK_D - TUBE_ID) / 2.0   # 90-degree countersink
    for (hx, hk) in HOLES_KY:
        cuts.append(cyl_z(hx, hk, TUBE_Z0 - 0.1, zt1 + 0.1, TUBE_ID))
        cone = Part.makeCone(TUBE_ID / 2.0, CSK_D / 2.0, depth, V(hx, -hk, zt1 - depth), V(0, 0, 1))
        cuts.append(cone)
    cuts.append(cyl_z(rx, rk, guide_z0 - 0.1, zt1 + 0.1, RESET_ID))
    return cut_all(cover, cuts)


def make_button_cap(x=BUTTON_X[0]):
    stem = pill_y(x, BUTTON_Z, CAP_STEM_KY[0], CAP_STEM_KY[1], CAP_STEM_W, CAP_STEM_H)
    flange = pill_y(x, BUTTON_Z, CAP_FLANGE_KY[0], CAP_FLANGE_KY[1], CAP_FLANGE_W, CAP_FLANGE_H)
    flange = flange.cut(box(x - 6, x + 6, 80, 90, BUTTON_Z - 5, CAP_FLANGE_ZMIN))
    pin = cyl_y(x, BUTTON_Z, CAP_PIN_KY[0] - 0.001, CAP_PIN_KY[1], CAP_PIN_D)
    return fuse_all([stem, flange, pin])


# ----------------------------------------------------------------------------
# GHOST / KEEP-OUT BODIES
# ----------------------------------------------------------------------------
def glass_box():
    return box(GLASS_X[0], GLASS_X[1], GLASS_KY[0], GLASS_KY[1], GLASS_Z[0], GLASS_Z[1])


def battery_box(swell=0.0):
    return box(BAT_C[0] - BAT_W / 2, BAT_C[0] + BAT_W / 2, BAT_C[1] - BAT_L / 2, BAT_C[1] + BAT_L / 2,
               BAT_Z0, BAT_Z0 + BAT_T + swell)


def fpc_corridor():
    # Spec numbers are inconsistent with the shell: the corridor reaches x=194.5 (inside the
    # right wall, which starts at INT_X[1]=193.8) and starts at z=-1.6 (inside the bezel plate,
    # whose back face is z=-1.45). Clip to the cavity, which is what the FPC can actually use.
    return box(FPC_X[0], min(FPC_X[1], INT_X[1]), FPC_KY[0], FPC_KY[1],
               max(FPC_Z[0], PLATE_Z[1]), FPC_Z[1])


def s4_body(s4_top):
    return box(S4_BODY_X[0], S4_BODY_X[1], S4_BODY_KY[0], S4_BODY_KY[1], PCB_TOP, s4_top)


# ----------------------------------------------------------------------------
# OUTPUT
# ----------------------------------------------------------------------------
def export_part(shape, name):
    shape.exportStep(os.path.join(OUT_DIR, name + ".step"))
    stl = os.path.join(OUT_DIR, name + ".stl")
    try:
        import MeshPart
        mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.02,
                                      AngularDeflection=math.radians(10), Relative=False)
        mesh.write(stl)
    except Exception as ex:  # fall back to Part's own tessellation
        print("MeshPart failed (%s), using Shape.exportStl" % ex)
        shape.exportStl(stl)


def rgba(r, g, b, a=255):
    return (r << 24) | (g << 16) | (b << 8) | a


def write_fcstd(path, items):
    """items: list of (name, shape, (r,g,b), transparency%). Writes colours via GuiDocument.xml."""
    doc = App.newDocument("NanoPage_case")
    names = []
    for name, shape, col, tr in items:
        o = doc.addObject("Part::Feature", name)
        o.Shape = shape
        names.append((o.Name, col, tr))
    doc.recompute()
    if os.path.exists(path):
        os.remove(path)
    doc.saveAs(path)
    App.closeDocument(doc.Name)

    # Colours live in Case/view_case.py: a headless run has no view objects to colour.


# ----------------------------------------------------------------------------
# CHECKS
# ----------------------------------------------------------------------------
def validity(name, shape):
    ok_valid = shape.isValid()
    nsol = len(shape.Solids)
    try:
        shape.check(True)
        chk = "ok"
    except Exception as ex:
        chk = "FAIL (%s)" % str(ex).splitlines()[0]
    good = ok_valid and nsol == 1 and chk == "ok" and shape.Volume > 0 and shape.ShapeType == "Solid"
    print("%-12s valid=%s solids=%d type=%s check=%s volume=%.2f mm3  %s" % (
        name, ok_valid, nsol, shape.ShapeType, chk, shape.Volume, "PASS" if good else "FAIL"))
    return good


def fit_check(parts, ref, keepouts):
    """parts: [(name, shape)], keepouts: [(name, shape or list of solids, intended_contact_note)]."""
    print("\n=== FIT CHECK (intersection volume, mm3; PASS if <= %.2f) ===" % TOL)
    allok = True
    for pname, pshape in parts:
        for kname, kshape in keepouts:
            if isinstance(kshape, list):
                v = sum(vol_common(pshape, s) for s in kshape)
            else:
                v = vol_common(pshape, kshape)
            ok = v <= TOL
            allok &= ok
            print("%-12s vs %-28s %10.2f  %s" % (pname, kname, v, "PASS" if ok else "FAIL"))
    return allok


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    ref, board, outline_wire = load_reference()
    s4_top, s4_src = find_s4_top(ref)
    print("S4 top Z = %.3f (source: %s); reset guide ends at Z = %.3f" % (
        s4_top, s4_src, s4_top + RESET_BELOW_TOP))

    shell = make_front_shell()
    cover = make_rear_cover(s4_top)
    cap = make_button_cap(BUTTON_X[0])
    caps = [make_button_cap(x) for x in BUTTON_X]

    print("\n=== SOLID VALIDITY ===")
    good = all([validity("front_shell", shell), validity("rear_cover", cover),
                validity("button_cap", cap)])

    export_part(shell, "front_shell")
    export_part(cover, "rear_cover")
    export_part(cap, "button_cap")

    # ghost bodies
    pcb_outline = Part.Face(outline_wire).extrude(V(0, 0, PCB_T))   # outline from the STEP z=0 face
    glass = glass_box()
    bat = battery_box()
    bat_keep = battery_box(BAT_SWELL)
    fpc = fpc_corridor()
    s4 = s4_body(s4_top)

    items = [
        ("front_shell", shell, (235, 235, 230), 0),
        ("rear_cover", cover, (200, 215, 235), 0),
        ("button_cap_1", caps[0], (240, 120, 60), 0),
        ("button_cap_2", caps[1], (240, 120, 60), 0),
        ("button_cap_3", caps[2], (240, 120, 60), 0),
        ("ghost_PCB_assembly", ref, (40, 140, 60), 20),
        ("ghost_display_glass", glass, (60, 120, 220), 50),
        ("ghost_battery_box", bat, (230, 60, 60), 50),
        ("ghost_FPC_corridor", fpc, (250, 220, 40), 70),
    ]
    write_fcstd(os.path.join(OUT_DIR, "NanoPage_case.FCStd"), items)

    parts = [("front_shell", shell), ("rear_cover", cover)] + \
            [("button_cap_%d" % (i + 1), c) for i, c in enumerate(caps)]
    keepouts = [
        ("FPC corridor", fpc),
        ("battery box (+swell)", bat_keep),
        ("display glass", glass),
        ("PCB assembly (STEP)", ref.Solids),
        ("S4 body (DNP, not in STEP)", s4),
        ("PCB outline prism (STEP)", pcb_outline),
    ]
    allok = fit_check(parts, ref, keepouts)

    print("\n=== PRINTED-PART INTERFERENCE (not a keep-out, sanity) ===")
    for cname, c in (("rear_cover", cover),) + tuple(("button_cap_%d" % (i + 1), x) for i, x in enumerate(caps)):
        v = vol_common(shell, c)
        ok = v <= TOL
        allok &= ok
        print("front_shell  vs %-28s %10.2f  %s" % (cname, v, "PASS" if ok else "FAIL"))

    # overall outer size
    bb = shell.BoundBox
    for c in (cover,) + tuple(caps):
        bb.add(c.BoundBox)
    print("\nOuter size: %.2f x %.2f x %.2f mm (X x Y x Z), Z %.2f..%.2f" % (
        bb.XLength, bb.YLength, bb.ZLength, bb.ZMin, bb.ZMax))
    print("Spec expects about 96.8 x 42.8 x 13.75 (caps stick out 0.6 beyond the top wall, so Y may be larger)")
    sbb = shell.BoundBox
    print("Shell only: %.2f x %.2f x %.2f mm" % (sbb.XLength, sbb.YLength, sbb.ZLength))
    print("Board outline taken from STEP z=0 face: area %.1f mm2 (incl. holes), used as a %.2f thick keep-out prism" % (
        Part.Face(outline_wire).Area, PCB_T))
    print("\nOVERALL: %s" % ("PASS" if (allok and good) else "FAIL"))


main()
