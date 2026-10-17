"""Side-by-side test: map A (text-only design) vs map B (building type + beats). Schema 0.2, meters, Y up, north = -Z."""
import json, os, sys

# Where generated maps go: set LEVEL_OUT to <assets root>/maps/<folder> (the editor only opens maps inside its assets root).
OUT = os.environ.get("LEVEL_OUT") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
LOOPS = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2]]  # -Z +Z -X +X -Y(bottom) +Y(top)
ROOM_CARVE = {"self": True, "structural": False, "nonStructural": False}


class Map:
    def __init__(self):
        self.world, self.detail, self.points, self.n = [], [], [], 0

    def _brush(self, mn, mx, tex, op="add"):
        self.n += 1
        (x0, y0, z0), (x1, y1, z1) = mn, mx
        v = [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0], [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]]
        t = tex if isinstance(tex, dict) else {"side": tex, "top": tex, "bottom": tex}
        faces = [{"id": i, "loop": l, "texture": t["bottom"] if i == 4 else t["top"] if i == 5 else t["side"]} for i, l in enumerate(LOOPS)]
        b = {"id": f"b{self.n}", "op": op, "vertices": v, "faces": faces}
        if op == "subtract":
            b["carve"] = ROOM_CARVE
        return b

    def mass(self, mn, mx, tex):  # solid building / ground volume (worldspawn, convex)
        self.world.append(self._brush(mn, mx, tex))

    def cut(self, mn, mx, wall="dev/wall", floor="dev/wood", ceil="dev/grey"):  # room / door / window carved out of the masses
        self.world.append(self._brush(mn, mx, {"side": wall, "bottom": floor, "top": ceil}, "subtract"))

    def det(self, mn, mx, tex):  # func_detail (never carved)
        self.detail.append(self._brush(mn, mx, tex))

    def stairs(self, x0, x1, z_from, z_dir, y0, y1, riser=0.24, tread=0.32, tex="dev/wood"):
        n = max(1, round((y1 - y0) / riser))
        r = (y1 - y0) / n
        for j in range(n):
            za, zb = z_from + z_dir * j * tread, z_from + z_dir * (j + 1) * tread
            self.det((x0, y0, min(za, zb)), (x1, y0 + (j + 1) * r, max(za, zb)), tex)

    def point(self, cls, origin, **kv):
        self.n += 1
        self.points.append({"id": f"e{self.n}", "classname": cls, "origin": list(origin), "angles": [0, 0, 0], "keyvalues": {k: str(v) for k, v in kv.items()}})

    def light(self, origin, color="1 0.85 0.6", brightness=2, radius=10):
        self.point("light_point", origin, color=color, brightness=brightness, radius=radius)

    def doc(self):
        ents = [{"id": "e_worldspawn", "classname": "worldspawn", "keyvalues": {}, "origin": [0, 0, 0], "angles": [0, 0, 0], "brushes": self.world}]
        if self.detail:
            ents.append({"id": "e_detail", "classname": "func_detail", "keyvalues": {}, "origin": [0, 0, 0], "angles": [0, 0, 0], "brushes": self.detail})
        return {"schemaVersion": "0.2", "entities": ents + self.points, "prefabInstances": []}

    # --- ray checks for beats: solid = (inside a worldspawn add brush and no subtractor) or inside a detail brush
    def solid(self, p):
        inside = lambda b: all(min(c[k] for c in b["vertices"]) < p[k] < max(c[k] for c in b["vertices"]) for k in range(3))
        if any(inside(b) for b in self.detail):
            return True
        return any(inside(b) for b in self.world if b["op"] == "add") and not any(inside(b) for b in self.world if b["op"] == "subtract")

    def visible(self, a, b, step=0.1):
        d = [b[k] - a[k] for k in range(3)]
        L = sum(x * x for x in d) ** 0.5
        for i in range(int(0.3 / step), int((L - 0.3) / step)):
            t = i * step / L
            if self.solid([a[k] + d[k] * t for k in range(3)]):
                return False, round(i * step, 1)
        return True, round(L, 1)


BRICK, FACADE, PLASTER, GROUND, PAVE = "building_blocks/brick_wall_pt_8", "building_blocks/brick_wall_pt_5", "dev/concrete", "dev/concrete", "dev/floor"


def map_a():
    """Text-only design, written without looking: garden in front of a two-storey box house, three routes to the study."""
    m = Map()
    m.mass((-30, -0.3, -20), (30, 0, 30), {"side": GROUND, "top": "dev/floor", "bottom": GROUND})        # site ground
    m.mass((-12, -4, -8), (12, 9, 8), {"side": BRICK, "top": "dev/grey", "bottom": BRICK})              # house, 2 storeys
    for x in (-12.4, 11.9):                                                                              # garden fence E / W
        m.mass((x, 0, 8), (x + 0.5, 1.5, 24), "dev/wall")
    m.mass((-12.4, 0, 24), (-2, 1.5, 24.5), "dev/wall"); m.mass((2, 0, 24), (12.4, 1.5, 24.5), "dev/wall")  # fence S with gate gap
    # ground floor: hall (centre), kitchen (W), living room (E); upper: corridor, study (E), bedroom (W)
    m.cut((-4, 0, -7.5), (4, 4.2, 7.5)); m.cut((-11.5, 0, -7.5), (-4.5, 4.2, 7.5)); m.cut((4.5, 0, -7.5), (11.5, 4.2, 7.5))
    m.cut((-11.5, 4.6, -7.5), (-4.5, 8.4, 7.5)); m.cut((-4, 4.6, -7.5), (4, 8.4, -2)); m.cut((4.5, 4.6, -7.5), (11.5, 8.4, 7.5))
    m.cut((-1, 0, 7.4), (1, 2.4, 8.1))                                                                  # front door
    for x0 in (-4.6, 3.9): m.cut((x0, 0, 2), (x0 + 0.7, 2.4, 3.4))                                      # hall -> kitchen / living
    for x0 in (-4.6, 3.9): m.cut((x0, 4.6, -6), (x0 + 0.7, 7, -4.6))                                    # corridor -> bedroom / study
    m.cut((-3, -0.5, -1.9), (-1.5, 4.7, 7.4))                                                           # stairwell opening hall -> corridor
    m.stairs(-3, -1.5, 6.8, -1, 0, 4.6)
    for x in (-9, -6.5, 6.5, 9): m.cut((x - 0.6, 1, 7.4), (x + 0.6, 2.6, 8.1)); m.cut((x - 0.6, 5.6, 7.4), (x + 0.6, 7.2, 8.1))  # windows
    m.cut((-11.5, -3.6, -7.5), (-4.5, -0.4, 0))                                                         # cellar under kitchen
    m.cut((-11.5, -0.5, -2), (-10, 0.1, 0)); m.stairs(-11.5, -10, -0.2, -1, -3.6, 0)                   # cellar stairs
    m.cut((-4.6, -1.5, -5), (3, -0.4, -4)); m.cut((2, -1.5, -5), (3, 0.05, -4))                         # vent cellar -> hall
    m.cut((9, 8.3, 4), (10.5, 9.1, 5.5))                                                                 # roof hatch over study
    m.det((12, 0, 0), (12.2, 9.2, 1), "dev/metal")                                                      # ladder E wall
    m.det((-1.2, 0, 16), (1.2, 0.6, 18.4), "dev/concrete"); m.det((-0.3, 0, 16.9), (0.3, 2, 17.5), "dev/concrete")  # fountain
    m.point("info_player_start", (0, 0.1, 28), angle=90)
    for p in ((0, 3.5, 0), (8, 7.5, 0), (-8, 3.5, 0), (-8, -1, -3)): m.light(p)
    return m


def map_b():
    """Building type: hotel particulier (U-plan house around a cour d'honneur, behind a street wall with a gate).
    Set pieces: grand stair hall with gallery, courtyard with fountain landmark, roof terrace.
    Beats: B1 from the gate the lit study window is visible across the courtyard; B2 the hall opens up two storeys with the stair
    climbing to the gallery; B3 service route: courtyard -> kitchen -> cellar -> vent into the hall; B4 roof route: ladder -> terrace
    overlooking the courtyard -> hatch into the study."""
    m = Map()
    m.mass((-30, -0.3, -12), (30, 0, 36), {"side": GROUND, "top": PAVE, "bottom": GROUND})            # site: street + courtyard
    m.mass((-16, -4, -6), (16, 9, 6), {"side": FACADE, "top": "dev/grey", "bottom": FACADE})           # corps de logis (main block)
    m.mass((-16, -4, 6), (-10, 9, 20), {"side": FACADE, "top": "dev/grey", "bottom": FACADE})          # west wing (service)
    m.mass((10, 0, 6), (16, 9, 20), {"side": FACADE, "top": "dev/grey", "bottom": FACADE})             # east wing (private)
    m.mass((10, 9, 6), (10.3, 9.9, 20), "dev/wall")                                                   # FIX B4: low balustrade on the courtyard side
    m.mass((15.7, 9, 6), (16, 10, 20), "dev/wall"); m.mass((10, 9, 19.7), (16, 10, 20), "dev/wall")
    m.mass((-16, 0, 24), (-3, 4.5, 24.6), BRICK); m.mass((3, 0, 24), (16, 4.5, 24.6), BRICK)            # FIX B1: 6 m gate
    m.mass((-3, 3.5, 24), (3, 4.5, 24.6), BRICK)                                                        # gate lintel
    for x0 in (-16, 15.6): m.mass((x0, 0, 20), (x0 + 0.4, 4.5, 24), BRICK)                             # courtyard corners
    # main block: grand stair hall (2 storeys) with gallery; dining W, salon E; bedroom W upstairs, antechamber E upstairs
    m.cut((-5, 0, -5), (5, 8.4, 5), wall="dev/wall", floor="dev/floor")
    m.cut((-15.5, 0, -5), (-5.5, 4.2, 5)); m.cut((5.5, 0, -5), (15.5, 4.2, 5))
    m.cut((-15.5, 4.6, -5), (-5.5, 8.4, 5)); m.cut((5.5, 4.6, -5), (15.5, 8.4, 5))
    m.det((-5, 4.2, -5), (5, 4.6, -2.5), "dev/wood")                                                    # gallery
    for x0, x1 in ((-5, -1.6), (1.6, 5)): m.det((x0, 4.6, -2.6), (x1, 5.6, -2.5), "dev/wood")          # gallery railing, gap for the stair
    m.stairs(-1.6, 1.6, 3.2, -1, 0, 4.4, riser=0.25, tread=0.32)                                         # FIX B2: axial grand stair up to the gallery
    m.cut((-1.3, 0, 4.9), (1.3, 3.4, 6.1))                                                               # front double door
    for x in (-7.5, -3.5, 3.5, 7.5):                                                                     # tall facade windows
        m.cut((x - 0.7, 1, 4.9), (x + 0.7, 3.4, 6.1)); m.cut((x - 0.7, 5.4, 4.9), (x + 0.7, 7.6, 6.1))
    for x0 in (-5.6, 4.9): m.cut((x0, 0, -0.8), (x0 + 0.7, 2.6, 0.8))                                   # hall -> dining / salon
    m.cut((4.9, 4.6, -4.6), (5.6, 7, -3))                                                                # gallery -> antechamber
    # east wing: library below, study above (window on the courtyard), terrace on the roof with a hatch
    m.cut((10.5, 0, 6.5), (15.5, 4.2, 19.5)); m.cut((10.5, 4.6, 6.5), (15.5, 8.4, 13))
    m.cut((12.3, 4.6, 4.9), (13.7, 7, 6.6))                                                             # antechamber -> study
    m.cut((9.9, 5.4, 8.5), (10.6, 7.6, 11))                                                             # STUDY WINDOW (beat B1)
    m.cut((9.9, 1, 9), (10.6, 3.2, 10.4)); m.cut((9.9, 1, 15), (10.6, 3.2, 16.4))                       # library windows
    m.cut((14, 8.3, 11.4), (15.2, 9.1, 12.6))                                                           # roof hatch -> study (beat B4)
    for i in range(12): m.det((12.5, 0.4 + i * 0.75, 20), (13.5, 0.5 + i * 0.75, 20.15), "dev/metal")  # ladder rungs (beat B4)
    # west wing: kitchen with a courtyard door, cellar below, vent to the hall (beat B3)
    m.cut((-15.5, 0, 6.5), (-10.5, 4.2, 19.5), floor="dev/floor"); m.cut((-10.6, 0, 16), (-9.9, 2.4, 17.4))
    m.cut((-13.7, 0, 4.9), (-12.3, 2.4, 6.6))                                                           # kitchen -> dining
    m.cut((-15.5, -3.6, 7), (-10.5, -0.4, 15), floor="dev/concrete", wall="dev/concrete")
    m.cut((-15.5, -0.5, 9.4), (-14, 0.1, 15)); m.stairs(-15.5, -14, 9.6, 1, -3.6, 0)
    m.cut((-12.5, -1.5, -3), (-11.5, -0.4, 7.1), wall="dev/metal"); m.cut((-12.5, -1.5, -3), (-3, -0.4, -2), wall="dev/metal")
    m.cut((-4, -1.5, -3), (-3, 0.05, -2), wall="dev/metal")                                             # vent opening in the hall floor
    # courtyard: fountain landmark on the axis between gate and front door
    m.det((-1.6, 0, 13.4), (1.6, 0.6, 13.7), "dev/concrete"); m.det((-1.6, 0, 16.3), (1.6, 0.6, 16.6), "dev/concrete")
    m.det((-1.6, 0, 13.7), (-1.3, 0.6, 16.3), "dev/concrete"); m.det((1.3, 0, 13.7), (1.6, 0.6, 16.3), "dev/concrete")
    m.det((-0.3, 0, 14.7), (0.3, 2.4, 15.3), "dev/concrete"); m.det((-0.7, 2.4, 14.6), (0.7, 2.7, 15.4), "dev/concrete")
    m.point("info_player_start", (0, 0.1, 27), angle=90)
    for p in ((0, 6.5, 0), (13, 7.5, 9.8), (-13, 3.5, 12), (-13, -1, 11), (13, 3.5, 13)): m.light(p)
    return m


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, m in (("a", map_a()), ("b", map_b())):
        json.dump(m.doc(), open(os.path.join(OUT, f"{name}.map.json"), "w"), indent=1)
        print(name, "brushes", len(m.world) + len(m.detail))
    b = map_b()
    beats = {"B1 start eye -> study window": ((0, 1.6, 27), (10.0, 6.5, 9.75)),
             "B2 hall door -> gallery door": ((0, 1.6, 4.5), (5.0, 5.8, -3.8)),
             "B4 terrace edge -> courtyard fountain": ((10.8, 10.6, 17), (0, 2.5, 15))}
    for k, (a, t) in beats.items():
        print(k, "visible=%s at %s m" % b.visible(a, t))
