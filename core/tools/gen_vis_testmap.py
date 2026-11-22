"""Generates the visibility test map: sections of different complexity, each in its own playable zone, 200 m apart along +X.

    python gen_vis_testmap.py <assets root> [maps/vis_test.map.json] [--seed N]

Writes the map (schema 0.2, meters, Y up) and `<map>.sections.json` (name, bounds, what it tests) that `vis_check` reads to report per section.
Every wall is a set of closed convex worldspawn boxes (jambs, lintel, no CSG), so the map is exactly what the visibility sees: no carving.

Sections (the first two are the cases that went wrong before: many door holes in a hallway):
  hallway     60 m hallway with 12 doors on each side, staggered, into 6 x 6 rooms, a few rooms with a second door into the next room
  enfilade    two rows of 8 rooms: doors on one axis (one long view through all of them) and doors alternating left / right (no view)
  rooms       6 x 6 rooms of 8 m, random doors between neighbours (many small closed spaces)
  maze        a 16 x 16 maze of 3 m cells, a few extra openings (corridors with many holes)
  angled      rooms divided by walls at 45 degrees, a 0.1 m thin wall, a 1 cm slit, a 5 cm hole, a wall with a 0.3 m door
  plaza       open air 48 x 48 m with a pillar grid, big blocks and a low wall (nothing hides much: little can be culled)
  floors      two storeys, an atrium void, stairs, balconies, floor holes
  hall        a big hall with closets round it, each with a door and a window slit
"""
import json
import math
import os
import random
import sys

LOOPS = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2]]
H = 3.5          # storey height of the rooms
T = 0.3          # wall thickness
DOOR_W, DOOR_H = 1.4, 2.4


class Level:
    def __init__(self, seed=1):
        self.world, self.detail, self.zones, self.points, self.sections = [], [], [], [], []
        self.n = 0
        self.rng = random.Random(seed)

    # ---- brushes
    def _brush(self, verts, tex):
        self.n += 1
        faces = [{"id": i, "loop": l, "texture": tex} for i, l in enumerate(LOOPS)]
        return {"id": f"b{self.n}", "op": "add", "vertices": [list(map(float, v)) for v in verts], "faces": faces}

    @staticmethod
    def _box(mn, mx):
        (x0, y0, z0), (x1, y1, z1) = mn, mx
        return [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]

    def box(self, mn, mx, tex="dev/grey", detail=False):
        if any(mx[k] - mn[k] < 0.004 for k in range(3)):
            return
        (self.detail if detail else self.world).append(self._brush(self._box(mn, mx), tex))

    def turned(self, cx, cz, length, thick, y0, y1, deg, tex="dev/grey"):
        """A wall box centred at (cx, cz), `length` along the direction deg (0 = +X), turned about Y."""
        c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        base = []
        for dx, dz in ((-length / 2, -thick / 2), (length / 2, -thick / 2), (length / 2, thick / 2), (-length / 2, thick / 2)):
            base.append((cx + dx * c - dz * s, cz + dx * s + dz * c))
        # the box vertex order of LOOPS: (x0,y0,z0) (x1,y0,z0) (x1,y1,z0) (x0,y1,z0) (x0,y0,z1) (x1,y0,z1) (x1,y1,z1) (x0,y1,z1)
        a, b, c2, d = base      # a = (x0,z0) b = (x1,z0) c2 = (x1,z1) d = (x0,z1)
        verts = [(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1]),
                 (d[0], y0, d[1]), (c2[0], y0, c2[1]), (c2[0], y1, c2[1]), (d[0], y1, d[1])]
        self.world.append(self._brush(verts, tex))

    # ---- walls with openings. along: 'x' (the wall runs along X, thin in Z) or 'z'
    def wall(self, along, a0, a1, fixed, t, y0, y1, openings=(), tex="dev/grey"):
        """openings: (from, to, top) along the wall (absolute coordinates), the opening reaches from y0 up to `top`."""
        ops = sorted(openings)
        cur = a0
        def seg(lo, hi, bottom, top):
            if hi - lo < 1e-4 or top - bottom < 1e-4:
                return
            if along == "x":
                self.box((lo, bottom, fixed - t / 2), (hi, top, fixed + t / 2), tex)
            else:
                self.box((fixed - t / 2, bottom, lo), (fixed + t / 2, top, hi), tex)
        for lo, hi, top in ops:
            seg(cur, lo, y0, y1)        # solid up to the opening
            seg(lo, hi, top, y1)        # lintel
            cur = hi
        seg(cur, a1, y0, y1)

    def room(self, x0, z0, x1, z1, openings, y0=0.0, y1=H, ceiling=True, tex="dev/grey"):
        """Four walls round [x0,x1] x [z0,z1] (the walls centred on the rectangle's edges). openings: dict side -> [(from, to, top)], sides 'n' (z0) 's' (z1) 'w' (x0) 'e' (x1)."""
        self.wall("x", x0 - T / 2, x1 + T / 2, z0, T, y0, y1, openings.get("n", ()), tex)
        self.wall("x", x0 - T / 2, x1 + T / 2, z1, T, y0, y1, openings.get("s", ()), tex)
        self.wall("z", z0 + T / 2, z1 - T / 2, x0, T, y0, y1, openings.get("w", ()), tex)
        self.wall("z", z0 + T / 2, z1 - T / 2, x1, T, y0, y1, openings.get("e", ()), tex)
        if ceiling:
            self.box((x0 - T / 2, y1, z0 - T / 2), (x1 + T / 2, y1 + 0.3, z1 + T / 2), tex)

    def floor(self, x0, z0, x1, z1, y=0.0, thick=0.5, tex="dev/concrete"):
        self.box((x0, y - thick, z0), (x1, y, z1), tex)

    # ---- sections
    def begin(self, name, note, x0, z0, x1, z1, y0=0.0, y1=H + 3):
        self.sections.append({"name": name, "note": note, "mn": [x0, y0, z0], "mx": [x1, y1, z1]})
        self.zones.append(self._brush(self._box((x0, y0 - 0.2, z0), (x1, y1, z1)), "dev/grey"))
        self.point("info_player_start", ((x0 + x1) / 2, 0.1, (z0 + z1) / 2))
        self.point("light_point", ((x0 + x1) / 2, H - 0.5, (z0 + z1) / 2), brightness="8", radius="40")

    def point(self, cls, origin, **kv):
        self.n += 1
        self.points.append({"id": f"e{self.n}", "classname": cls, "origin": list(map(float, origin)), "angles": [0, 0, 0], "keyvalues": {k: str(v) for k, v in kv.items()}})

    def door(self, c, w=DOOR_W, top=DOOR_H):
        return (c - w / 2, c + w / 2, top)

    # 1. hallway with many door holes
    def hallway(self, ox):
        L, HW, R = 60.0, 4.0, 6.0
        self.begin("hallway", "60 m hallway, 12 doors on each side, staggered, 6 x 6 rooms behind them", ox - 2, -R - T - 2, ox + L + 2, HW + R + T + 2)
        self.floor(ox - 2, -R - T - 2, ox + L + 2, HW + R + T + 2)
        n = 12
        step = L / n
        north_doors = [self.door(ox + step * (i + 0.5)) for i in range(n)]
        south_doors = [self.door(ox + step * (i + 0.5) + (0.4 if i % 3 == 0 else 0)) for i in range(n)]
        self.wall("x", ox - T / 2, ox + L + T / 2, 0, T, 0, H, north_doors)      # hallway north wall (z = 0)
        self.wall("x", ox - T / 2, ox + L + T / 2, HW, T, 0, H, south_doors)     # hallway south wall (z = HW)
        self.wall("z", T / 2, HW - T / 2, ox, T, 0, H)
        self.wall("z", T / 2, HW - T / 2, ox + L, T, 0, H)
        self.box((ox - T / 2, H, -T / 2), (ox + L + T / 2, H + 0.3, HW + T / 2), "dev/grey")
        for i in range(n):
            x0, x1 = ox + step * i + 0.4, ox + step * (i + 1) - 0.4
            xm = (x0 + x1) / 2
            # north room (z from -R to 0), south room (z from HW to HW + R)
            self.room(x0, -R, x1, 0, {}, ceiling=True)
            self.room(x0, HW, x1, HW + R, {}, ceiling=True)
            self.point("light_point", (xm, H - 0.5, -R / 2), brightness="5", radius="12")
            self.point("light_point", (xm, H - 0.5, HW + R / 2), brightness="5", radius="12")

    # 2. enfilade
    def enfilade(self, ox):
        R, n = 8.0, 8
        self.begin("enfilade", "two rows of 8 rooms: doors on one axis (one long view) and alternating (no view)", ox - 2, -2, ox + n * R + 2, 2 * R + 14)
        self.floor(ox - 2, -2, ox + n * R + 2, 2 * R + 14)
        for row, z0 in enumerate((0.0, R + 6)):
            for i in range(n):
                x0, x1 = ox + i * R, ox + (i + 1) * R
                op = {}
                if i < n - 1:
                    cz = z0 + R / 2 if row == 0 else z0 + (R * 0.25 if i % 2 == 0 else R * 0.75)
                    op["e"] = [self.door(cz)]
                if i > 0:
                    prev = z0 + R / 2 if row == 0 else z0 + (R * 0.25 if (i - 1) % 2 == 0 else R * 0.75)
                    op["w"] = [self.door(prev)]
                self.room(x0, z0, x1, z0 + R, op)
                self.point("light_point", ((x0 + x1) / 2, H - 0.5, z0 + R / 2), brightness="4", radius="10")

    # 3. rooms with random doors
    def rooms(self, ox):
        n, R = 6, 8.0
        self.begin("rooms", "6 x 6 rooms of 8 m with random doors between neighbours", ox - 2, -2, ox + n * R + 2, n * R + 2)
        self.floor(ox - 2, -2, ox + n * R + 2, n * R + 2)
        doorE = [[self.rng.random() < 0.5 for _ in range(n)] for _ in range(n)]
        doorS = [[self.rng.random() < 0.5 for _ in range(n)] for _ in range(n)]
        for gx in range(n):
            for gz in range(n):
                x0, z0 = ox + gx * R, gz * R
                op = {}
                if gx < n - 1 and doorE[gx][gz]:
                    op["e"] = [self.door(z0 + R / 2 + (gx % 3 - 1))]
                if gx > 0 and doorE[gx - 1][gz]:
                    op["w"] = [self.door(z0 + R / 2 + ((gx - 1) % 3 - 1))]
                if gz < n - 1 and doorS[gx][gz]:
                    op["s"] = [self.door(x0 + R / 2 + (gz % 3 - 1))]
                if gz > 0 and doorS[gx][gz - 1]:
                    op["n"] = [self.door(x0 + R / 2 + ((gz - 1) % 3 - 1))]
                self.room(x0, z0, x0 + R, z0 + R, op)
                if (gx + gz) % 2 == 0:
                    self.point("light_point", (x0 + R / 2, H - 0.5, z0 + R / 2), brightness="4", radius="10")

    # 4. maze
    def maze(self, ox):
        n, C = 16, 3.0
        self.begin("maze", "16 x 16 maze of 3 m cells, a spanning tree plus 30 extra openings", ox - 2, -2, ox + n * C + 2, n * C + 2)
        self.floor(ox - 2, -2, ox + n * C + 2, n * C + 2)
        # spanning tree by randomised depth first search; open[(a, b)] = passage
        seen = {(0, 0)}
        stack = [(0, 0)]
        passage = set()
        while stack:
            x, z = stack[-1]
            nb = [(x + dx, z + dz) for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)) if 0 <= x + dx < n and 0 <= z + dz < n and (x + dx, z + dz) not in seen]
            if not nb:
                stack.pop()
                continue
            c = self.rng.choice(nb)
            seen.add(c)
            passage.add((min((x, z), c), max((x, z), c)))
            stack.append(c)
        for _ in range(30):
            x, z = self.rng.randrange(n - 1), self.rng.randrange(n - 1)
            c = (x + 1, z) if self.rng.random() < 0.5 else (x, z + 1)
            passage.add(((x, z), c))
        for x in range(n + 1):          # walls along Z at x = const between cell columns x-1 and x
            for z in range(n):
                if x in (0, n) or ((x - 1, z), (x, z)) not in passage:
                    self.box((ox + x * C - 0.1, 0, z * C), (ox + x * C + 0.1, H, (z + 1) * C), "dev/grey")
        for z in range(n + 1):
            for x in range(n):
                if z in (0, n) or ((x, z - 1), (x, z)) not in passage:
                    self.box((ox + x * C, 0, z * C - 0.1), (ox + (x + 1) * C, H, z * C + 0.1), "dev/grey")
        self.box((ox - 0.1, H, -0.1), (ox + n * C + 0.1, H + 0.3, n * C + 0.1), "dev/grey")
        for x in range(0, n, 3):
            for z in range(0, n, 3):
                self.point("light_point", (ox + (x + 0.5) * C, H - 0.5, (z + 0.5) * C), brightness="3", radius="9")

    # 5. angled / thin / cracks
    def angled(self, ox):
        self.begin("angled", "45 degree walls, a 0.1 m wall, a 1 cm slit, a 5 cm hole, a 0.3 m door", ox - 2, -2, ox + 60 + 2, 20 + 2)
        self.floor(ox - 2, -2, ox + 62, 22)
        # shell: one hall 60 x 20, ceiling
        self.room(ox, 0, ox + 60, 20, {})
        # 1. a 45 degree wall across the first 20 m square, closed (the corners touch the shell)
        self.turned(ox + 10, 10, 20 * math.sqrt(2) - 0.4, 0.3, 0, H, 45)
        # 2. a 45 degree wall with a 1 m opening in the middle: the two sides see each other
        half = (20 * math.sqrt(2) - 0.4 - 1.0) / 2
        for sgn in (-1, 1):
            off = sgn * (half / 2 + 0.5)
            self.turned(ox + 30 + off * math.cos(math.radians(-45)), 10 + off * math.sin(math.radians(-45)), half, 0.3, 0, H, -45)
        # 3. a thin wall across x = ox + 40.3 with a 0.3 m wide door
        self.wall("z", 0.1, 19.9, ox + 40.3, 0.1, 0, H, [(9.85, 10.15, 2.4)])
        # 4. a 1 cm slit between two wall brushes at x = ox + 46
        self.box((ox + 45.5, 0, 0.1), (ox + 45.99, H, 19.9), "dev/grey")
        self.box((ox + 46.0, 0, 0.1), (ox + 46.5, H, 19.9), "dev/grey")
        # 5. a wall with a 5 cm square hole at x = ox + 52
        self.wall("z", 0.1, 19.9, ox + 52, 0.3, 0, H, [(9.0, 9.05, 1.55)])
        self.box((ox + 51.85, 0, 8.95), (ox + 52.15, 1.5, 9.0), "dev/grey")
        # lights
        for x in (5, 15, 25, 35, 43, 49, 56):
            self.point("light_point", (ox + x, H - 0.5, 10), brightness="5", radius="12")

    # 6. plaza
    def plaza(self, ox):
        S = 48.0
        self.begin("plaza", "open air 48 x 48 m: pillar grid, big blocks, a low wall", ox - 2, -2, ox + S + 2, S + 2, y1=8)
        self.floor(ox - 2, -2, ox + S + 2, S + 2)
        for x in range(4, int(S), 8):
            for z in range(4, int(S), 8):
                self.box((ox + x - 0.4, 0, z - 0.4), (ox + x + 0.4, 6, z + 0.4), "dev/concrete")
        for bx, bz, w, d, h in ((10, 10, 8, 4, 5), (30, 14, 6, 10, 6), (14, 32, 12, 5, 4), (36, 36, 4, 4, 7)):
            self.box((ox + bx, 0, bz), (ox + bx + w, h, bz + d), "dev/wall")
        self.box((ox + 6, 0, 22), (ox + 42, 1.2, 22.4), "dev/wall")
        self.point("light_directional", (ox, 20, 0), brightness="2")

    # 7. two storeys
    def floors(self, ox):
        W, D = 36.0, 24.0
        self.begin("floors", "two storeys, an atrium void, stairs, balconies, floor holes", ox - 2, -2, ox + W + 2, D + 2, y1=2 * H + 3)
        self.floor(ox - 2, -2, ox + W + 2, D + 2)
        self.room(ox, 0, ox + W, D, {"s": [self.door(ox + 4, 2.4, 2.4)]}, y0=0, y1=2 * H + 0.5, ceiling=True)
        # ground floor partitions
        for k, x in enumerate((ox + 12, ox + 24)):
            self.wall("z", 0.15, D - 0.15, x, 0.2, 0, H, [self.door(6 + 2 * k), self.door(18)])
        # the first floor slab with an atrium void in the middle and a stair opening
        s0, s1 = ox, ox + W
        y = H + 0.3
        self.box((s0, H, 0), (s1, y, 6), "dev/concrete")                       # north strip
        self.box((s0, H, D - 6), (s1, y, D), "dev/concrete")                   # south strip
        self.box((s0, H, 6), (ox + 12, y, D - 6), "dev/concrete")              # west block
        self.box((ox + 24, H, 6), (s1, y, D - 6), "dev/concrete")              # east block (the middle 12 m is the void)
        self.box((ox + 12, H, 6), (ox + 14, y, 8), "dev/concrete")
        # balcony rails along the void
        self.box((ox + 12, y, 6), (ox + 24, y + 1.1, 6.2), "dev/wall")
        self.box((ox + 12, y, D - 6.2), (ox + 24, y + 1.1, D - 6), "dev/wall")
        # first floor partitions with doors
        for x in (ox + 6, ox + 30):
            self.wall("z", 0.15, D - 0.15, x, 0.2, y, y + H - 0.5, [self.door(6), self.door(18)])
        # stairs up from the ground floor (west, against the north wall)
        for j in range(14):
            self.box((ox + 1, j * 0.25, 1 + j * 0.3), (ox + 3, (j + 1) * 0.25, 1.3 + j * 0.3), "dev/wood", detail=True)
        for x in (4, 16, 28):
            self.point("light_point", (ox + x, H - 0.5, 12), brightness="5", radius="14")
            self.point("light_point", (ox + x, y + 2.8, 12), brightness="5", radius="14")

    # 8. hall with closets
    def hall(self, ox):
        W, D = 40.0, 40.0
        self.begin("hall", "a big hall with closets round it, each with a door and a window slit", ox - 2, -2, ox + W + 2, D + 2)
        self.floor(ox - 2, -2, ox + W + 2, D + 2)
        self.room(ox, 0, ox + W, D, {"n": [self.door(ox + 20, 4, 3)]}, ceiling=True)
        # a row of closets (4 x 4) along the inside of the walls; each has a door into the hall and a slit window 0.1 m wide into the next closet
        k = 0
        for side in range(4):
            for i in range(8):
                u = 2 + i * 4.5
                if side == 0:
                    x0, z0, x1, z1 = ox + u, 0.15, ox + u + 4, 4.15
                    wall_side, op = "s", [self.door(x0 + 2)]
                elif side == 1:
                    x0, z0, x1, z1 = ox + u, D - 4.15, ox + u + 4, D - 0.15
                    wall_side, op = "n", [self.door(x0 + 2)]
                elif side == 2:
                    x0, z0, x1, z1 = ox + 0.15, u, ox + 4.15, u + 4
                    wall_side, op = "e", [self.door(z0 + 2)]
                else:
                    x0, z0, x1, z1 = ox + W - 4.15, u, ox + W - 0.15, u + 4
                    wall_side, op = "w", [self.door(z0 + 2)]
                self.room(x0, z0, x1, z1, {wall_side: op}, ceiling=True)
                k += 1
        self.point("light_point", (ox + 20, H - 0.5, 20), brightness="10", radius="30")

    # ---- output
    def doc(self):
        zone = {"id": "e_zones", "classname": "editor_playable_area", "keyvalues": {}, "origin": [0, 0, 0], "angles": [0, 0, 0], "brushes": self.zones}
        ents = [{"id": "e_worldspawn", "classname": "worldspawn", "keyvalues": {"_sky_auto": "0"}, "origin": [0, 0, 0], "angles": [0, 0, 0], "brushes": self.world}]
        if self.detail:
            ents.append({"id": "e_detail", "classname": "func_detail", "keyvalues": {}, "origin": [0, 0, 0], "angles": [0, 0, 0], "brushes": self.detail})
        ents.append(zone)
        return {"schemaVersion": "0.2", "entities": ents + self.points, "prefabInstances": []}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    seed = 1
    if "--seed" in sys.argv:
        seed = int(sys.argv[sys.argv.index("--seed") + 1])
        args = [a for a in args if a != str(seed)]
    if not args:
        print(__doc__)
        return 2
    root = args[0]
    rel = args[1] if len(args) > 1 else "maps/vis_test.map.json"
    lv = Level(seed)
    x = 0.0
    for build, width in ((lv.hallway, 70), (lv.enfilade, 80), (lv.rooms, 60), (lv.maze, 56), (lv.angled, 70), (lv.plaza, 56), (lv.floors, 46), (lv.hall, 50)):
        build(x)
        x += 200.0
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(lv.doc(), f)
    with open(path[:-len(".map.json")] + ".sections.json" if path.endswith(".map.json") else path + ".sections.json", "w") as f:
        json.dump(lv.sections, f, indent=1)
    print(f"{path}: {len(lv.world)} world brushes, {len(lv.detail)} detail, {len(lv.zones)} zones, {len(lv.points)} points")
    return 0


if __name__ == "__main__":
    sys.exit(main())
