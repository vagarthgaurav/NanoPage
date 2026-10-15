# Open the case model in the FreeCAD GUI and colour it.
# Run: COIN_GL_NO_CURRENT_CONTEXT_CHECK=1 QT_QPA_PLATFORM=xcb freecad Case/view_case.py
import os

import FreeCAD
import FreeCADGui

HERE = os.path.dirname(os.path.abspath(__file__))
FCSTD = os.path.join(HERE, "out", "NanoPage_case.FCStd")

# name: (r, g, b, transparency %)
STYLE = {
    "front_shell": (0.85, 0.85, 0.88, 0),
    "rear_cover": (0.70, 0.78, 0.90, 0),
    "button_cap_1": (0.95, 0.60, 0.20, 0),
    "button_cap_2": (0.95, 0.60, 0.20, 0),
    "button_cap_3": (0.95, 0.60, 0.20, 0),
    "ghost_PCB_assembly": (0.15, 0.55, 0.30, 0),
    "ghost_display_glass": (0.10, 0.10, 0.12, 30),
    "ghost_battery_box": (0.90, 0.80, 0.20, 40),
    "ghost_FPC_corridor": (0.90, 0.30, 0.30, 70),
}

doc = FreeCAD.openDocument(FCSTD)
FreeCADGui.ActiveDocument = FreeCADGui.getDocument(doc.Name)
for name, (r, g, b, tr) in STYLE.items():
    obj = doc.getObject(name)
    if obj is not None:
        # Objects made by a headless run open hidden, so switch them on.
        obj.ViewObject.Visibility = True
        obj.ViewObject.ShapeColor = (r, g, b)
        obj.ViewObject.Transparency = tr
FreeCADGui.activeDocument().activeView().viewIsometric()
FreeCADGui.SendMsgToActiveView("ViewFit")
