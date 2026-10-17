"""Mansion v4: big by extent, not by blown-up parts. Human-scale elements (storey 6.5 m, doors 3.5 x 4.5, windows 2.2 x 3.6
glazed, corridors 6 m, rooms 14-22 m) repeated over a 300 m main block (5 floors), wings and end pavilions (4 floors); a few hero
spaces (entrance hall, library, ballroom, chapel). Built additively: walls with openings, floor slabs, stair towers (no CSG).
Gardens are a sequence of garden rooms. Gated critical path, verified with reach.py per lock state.
Meters, Y up, north = -Z, the house faces south (+Z)."""
import json, math, os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapgen import Map, OUT
from mansion import hull, roof, prism, cone
import reach

WALL, IWALL, ROOF, PATH, LAWN, HEDGE, STONE, GLASS, WOOD, TRIG = "dev/grey", "dev/wall", "dev/metal", "dev/concrete", "dev/floor", "dev/floor", "dev/concrete", "dev/glass", "dev/wood", "dev/grate"
S, FL, T, TI, CW, BAY = 6.5, 1.5, 1.5, 1.0, 6.0, 6.0
DW, DH = 3.5, 4.5
BOUNDS = (-345, 345, -350, 700)
def Fy(k): return FL + k * S


class Level(Map):
    def __init__(self):
        super().__init__(); self.extra = []

    def brush_ent(self, cls, boxes, tex=TRIG, **kv):
        self.n += 1
        c = [(boxes[0][0][k] + boxes[0][1][k]) / 2 for k in range(3)]
        self.extra.append({"id": f"e{self.n}", "classname": cls, "keyvalues": {k: str(v) for k, v in kv.items()},
                           "origin": c, "angles": [0, 0, 0], "brushes": [self._brush(a, b, tex) for a, b in boxes]})

    def door(self, name, leaf, tex=WOOD): self.brush_ent("func_door", [leaf], tex, targetname=name, time=1, targetLocation="0 0 0")

    def doc(self):
        d = super().doc(); d["entities"] += self.extra; return d


def rect_minus(r, holes):
    rects = [r]
    for h in holes:
        nxt = []
        for a0, b0, a1, b1 in rects:
            if h[2] <= a0 or h[0] >= a1 or h[3] <= b0 or h[1] >= b1: nxt.append((a0, b0, a1, b1)); continue
            if h[0] > a0: nxt.append((a0, b0, h[0], b1))
            if h[2] < a1: nxt.append((h[2], b0, a1, b1))
            ua, ub = max(a0, h[0]), min(a1, h[2])
            if h[1] > b0: nxt.append((ua, b0, ub, h[1]))
            if h[3] < b1: nxt.append((ua, h[3], ub, b1))
        rects = nxt
    return rects


class Bld:
    """One building part. axis 'x': rooms line up along x (u = x, v = z); axis 'z': u = z, v = x. The corridor runs along u at
    v = cv. segs: [(u0, u1, kind, fa, fb)] kind 'rooms' or a hero space that is open from floor fa to floor fb (rooms above).
    slots: designated room slots {u0, u1, side ('lo'/'hi' of the corridor), kind, floors}. sides: per world side N/S/W/E:
    {own, spans, win, doors [(center, floors, w, h)]}."""
    def __init__(self, m, x0, z0, x1, z1, n, axis, cv, sides, segs, slots=(), seed=1, roofspec=None, xwall_floors=None):
        self.m, self.x0, self.z0, self.x1, self.z1, self.n, self.axis, self.cv = m, x0, z0, x1, z1, n, axis, cv
        self.sides, self.segs, self.slots, self.rng, self.roofspec = sides, segs, list(slots), random.Random(seed), roofspec
        self.xwall_floors = xwall_floors or {}
        own = {k: sides.get(k, {}).get("own", True) for k in "NSWE"}
        ix0, ix1 = x0 + (T if own["W"] else 0), x1 - (T if own["E"] else 0)
        iz0, iz1 = z0 + (T if own["N"] else 0), z1 - (T if own["S"] else 0)
        self.iu0, self.iu1, self.iv0, self.iv1 = (ix0, ix1, iz0, iz1) if axis == "x" else (iz0, iz1, ix0, ix1)

    def box(self, u0, v0, u1, v1, y0, y1, tex, det=False):
        u0, u1, v0, v1 = min(u0, u1), max(u0, u1), min(v0, v1), max(v0, v1)
        if u1 - u0 < 1e-3 or v1 - v0 < 1e-3 or y1 - y0 < 1e-3: return
        a, b = ((u0, y0, v0), (u1, y1, v1)) if self.axis == "x" else ((v0, y0, u0), (v1, y1, u1))
        (self.m.det if det else self.m.mass)(a, b, tex)

    def wall(self, along_u, c0, c1, s0, s1, y0, y1, holes, tex=IWALL):
        """A wall along u (or along v) between c0..c1 across it, s0..s1 along it, with holes [(a, b, ya, yb)] along it."""
        bx = (lambda a, b, ya, yb: self.box(a, c0, b, c1, ya, yb, tex)) if along_u else (lambda a, b, ya, yb: self.box(c0, a, c1, b, ya, yb, tex))
        p = s0
        for a, b, ya, yb in sorted((max(a, s0), min(b, s1), ya, yb) for a, b, ya, yb in holes if b > s0 and a < s1):
            bx(p, a, y0, y1); bx(a, b, y0, ya); bx(a, b, yb, y1); p = max(p, b)
        bx(p, s1, y0, y1)

    def ext_walls(self):
        m, F, H = self.m, [Fy(k) for k in range(self.n + 1)], Fy(self.n)
        for side in "NSWE":
            sp = self.sides.get(side, {})
            if not sp.get("own", True): continue
            along_x = side in "NS"
            c0, c1 = {"N": (self.z0, self.z0 + T), "S": (self.z1 - T, self.z1), "W": (self.x0, self.x0 + T), "E": (self.x1 - T, self.x1)}[side]
            spans = sp.get("spans", [((self.x0, self.x1) if along_x else (self.z0, self.z1))])
            for k in range(self.n):
                y0 = F[k]; y1 = F[k + 1] if k + 1 < self.n else H
                doors = [(c - w / 2, c + w / 2, y0, y0 + h) for c, fl, w, h in sp.get("doors", []) if k in fl]
                wins = []
                for a, b in sp.get("win", []):
                    nb = int((b - a) // BAY); pad = (b - a - nb * BAY) / 2
                    for i in range(nb):
                        c = a + pad + BAY * (i + 0.5)
                        if any(c + 1.1 > d0 - 1 and c - 1.1 < d1 + 1 for d0, d1, _, _ in doors): continue
                        ya = y0 + (1.0 if k == 0 else 1.2); wins.append((c - 1.1, c + 1.1, ya, ya + 3.6))
                for s0, s1 in spans:
                    lo, hi = (c0, c1)
                    def wbox(a, b, ya, yb, tex=WALL):
                        if b - a < 1e-3 or yb - ya < 1e-3: return
                        m.mass((a, ya, lo), (b, yb, hi), tex) if along_x else m.mass((lo, ya, a), (hi, yb, b), tex)
                    p = s0
                    for a, b, ya, yb in sorted((max(a, s0), min(b, s1), ya, yb) for a, b, ya, yb in wins + doors if b > s0 and a < s1):
                        wbox(p, a, y0, y1); wbox(a, b, y0, ya); wbox(a, b, yb, y1); p = max(p, b)
                    wbox(p, s1, y0, y1)
                    for a, b, ya, yb in wins:
                        if a >= s0 and b <= s1:
                            g0, g1 = (lo + hi) / 2 - 0.1, (lo + hi) / 2 + 0.1
                            m.det((a, ya, g0), (b, yb, g1), GLASS) if along_x else m.det((g0, ya, a), (g1, yb, b), GLASS)
                            m.det((a - 0.3, ya - 0.3, lo - 0.4 if side in "NW" else hi), (b + 0.3, ya, lo if side in "NW" else hi + 0.4), STONE) if along_x else \
                                m.det((lo - 0.4 if side in "NW" else hi, ya - 0.3, a - 0.3), (lo if side in "NW" else hi + 0.4, ya, b + 0.3), STONE)

    def plan_slots(self):
        """Room slots per rooms-capable segment and side: designated first, the rest split into 14-22 m rooms."""
        out = []
        for u0, u1, kind, fa, fb in self.segs:
            for side in ("lo", "hi"):
                des = sorted([s for s in self.slots if s["side"] == side and s["u0"] >= u0 - 1e-6 and s["u1"] <= u1 + 1e-6], key=lambda s: s["u0"])
                p = u0
                for d in des + [{"u0": u1, "u1": u1, "kind": None}]:
                    a = p
                    while d["u0"] - a > 1e-6:
                        rem = d["u0"] - a; w = self.rng.uniform(14, 22)
                        if rem - w < 12: w = rem
                        out.append({"u0": a, "u1": a + w, "side": side, "kind": "room", "floors": None, "seg": (u0, u1)}); a += w
                    if d["kind"]: out.append(dict(d, seg=(u0, u1)))
                    p = d["u1"]
        return out

    def build(self):
        m, n, F = self.m, self.n, [Fy(k) for k in range(self.n + 1)]
        H = F[n]
        m.mass((self.x0, -0.5, self.z0), (self.x1, FL, self.z1), STONE)
        self.ext_walls()
        cv0, cv1 = self.cv - CW / 2, self.cv + CW / 2
        slots = self.plan_slots()
        hero = lambda seg, k: seg[2] != "rooms" and seg[3] <= k < seg[4]
        stairs = [s for s in slots if s["kind"] == "stair"]
        # slabs (floor levels 1..n-1 and the roof slab at the top)
        for k in range(1, n + 1):
            for seg in self.segs:
                holes = []
                if seg[2] != "rooms" and seg[3] < k < seg[4]: continue
                if k < n:
                    for s in stairs:
                        if s["seg"] == (seg[0], seg[1]):
                            v0, v1 = (self.iv0, cv0 - TI) if s["side"] == "lo" else (cv1 + TI, self.iv1)
                            holes.append((s["u0"], v0, s["u1"], v1))
                for r in rect_minus((seg[0], self.iv0, seg[1], self.iv1), holes):
                    self.box(r[0], r[1], r[2], r[3], F[k] - 0.5, F[k], WOOD if k < n else ROOF)
        # interior walls per floor
        for k in range(n):
            y0, y1 = F[k], F[k + 1] - 0.5
            for seg in self.segs:
                u0, u1 = seg[0], seg[1]
                # cross walls of hero spaces (both ends), with the corridor opening on every floor unless closed there
                if seg[2] != "rooms":
                    for cu in (u0, u1):
                        if cu in (self.iu0, self.iu1): continue
                        c0, c1 = (cu, cu + TI) if cu == u0 else (cu - TI, cu)
                        holes = [] if k in self.xwall_floors.get((seg[2], cu), ()) else [(cv0, cv1, y0, y0 + 5.0)]
                        self.wall(False, c0, c1, self.iv0, self.iv1, y0, F[k + 1], holes)
                if hero(seg, k): continue
                ss = [s for s in slots if s["seg"] == (u0, u1)]
                for side in ("lo", "hi"):
                    row = sorted([s for s in ss if s["side"] == side], key=lambda s: s["u0"])
                    c0, c1 = (cv0 - TI, cv0) if side == "lo" else (cv1, cv1 + TI)
                    holes = []
                    for s in row:
                        mid = (s["u0"] + s["u1"]) / 2
                        live = s["floors"] is None or k in s["floors"]
                        w = CW if (live and s["kind"] == "link") else DW
                        holes.append((mid - w / 2, mid + w / 2, y0, y0 + (5.0 if w == CW else DH)))
                    self.wall(True, c0, c1, u0, u1, y0, F[k + 1], holes)
                    va, vb = (self.iv0, cv0 - TI) if side == "lo" else (cv1 + TI, self.iv1)
                    ext = va if side == "lo" else vb
                    for s0, s1 in zip(row, row[1:]):
                        b = s0["u1"]; plain = {s0["kind"], s1["kind"]} & {"stair", "link"}
                        en = [] if plain else [(min(ext + 2.5, ext - 6), max(ext + 2.5, ext - 6) if False else 0, 0, 0)]
                        hv = (va + 2.5, va + 6.0) if side == "lo" else (vb - 6.0, vb - 2.5)
                        self.wall(False, b - TI / 2, b + TI / 2, va, vb, y0, F[k + 1], [] if plain else [(hv[0], hv[1], y0, y0 + DH)])
                    for s in row:
                        live = s["floors"] is None or k in s["floors"]
                        kind = s["kind"] if live else "room"
                        self.furnish(s, side, k, kind, va, vb, y0)
        for seg in self.segs:
            if seg[2] != "rooms": HERO[seg[2]](self, seg)
        if self.roofspec: self.roofspec(self)

    def furnish(self, s, side, k, kind, va, vb, y0):
        u0, u1, rng = s["u0"] + TI / 2, s["u1"] - TI / 2, self.rng
        if kind == "stair":
            self.stairwell(s, side, k); return
        if kind in ("link",): return
        ext = va if side == "lo" else vb; d = 1 if side == "lo" else -1      # d: from the exterior wall inward
        if kind == "dining":
            self.box(u0 + 6, ext + d * 6, u1 - 6, ext + d * 9, y0, y0 + 1.1, WOOD, True); return
        if kind == "garden_salon":
            for uu in (u0 + 4, u1 - 8): self.box(uu, ext + d * 3, uu + 4, ext + d * 5, y0, y0 + 1.0, WOOD, True)
            return
        f = ["salon", "study", "bedroom", "bedroom", "attic"][min(k, 4)]
        mid = (u0 + u1) / 2
        if f == "salon":
            self.box(mid - 3, ext + d * 3, mid + 3, ext + d * 4.5, y0, y0 + 1.0, WOOD, True); self.box(mid - 1.5, ext + d * 6, mid + 1.5, ext + d * 8, y0, y0 + 0.9, WOOD, True)
        elif f == "study":
            self.box(u0 + 1, ext + d * 0.0, u0 + 7, ext + d * 1.0, y0, y0 + 4.0, WOOD, True); self.box(mid - 1.5, ext + d * 5, mid + 1.5, ext + d * 6.5, y0, y0 + 1.2, WOOD, True)
        elif f == "bedroom":
            self.box(u0 + 2, ext + d * 1, u0 + 5, ext + d * 5, y0, y0 + 1.2, WOOD, True); self.box(u1 - 2, ext + d * 1, u1 - 1, ext + d * 4, y0, y0 + 4, WOOD, True)
        else:
            for i in range(3):
                uu = rng.uniform(u0 + 2, u1 - 4); vv = ext + d * rng.uniform(2, 9); self.box(uu, vv, uu + 2, vv + 2, y0, y0 + 2, WOOD, True)

    def stairwell(self, s, side, k):
        """Switchback tower: landing by the corridor at each floor, flight A to a half landing, flight B up to the next floor."""
        cv0, cv1 = self.cv - CW / 2, self.cv + CW / 2
        vb_ = (cv0 - TI) if side == "lo" else (cv1 + TI); vf = self.iv0 if side == "lo" else self.iv1; d = -1 if side == "lo" else 1
        u0, u1 = s["u0"] + TI / 2, s["u1"] - TI / 2; um = (u0 + u1) / 2
        y = Fy(k); r, tread, nsteps = S / 26, 0.4, 13
        if k > 0: self.box(u0, vb_, u1, vb_ + d * 4.5, y - 0.5, y, WOOD, True)
        if k >= self.n - 1: return
        va = vb_ + d * 4.5
        for j in range(nsteps): self.box(u0, va + d * j * tread, um, va + d * (j + 1) * tread, y, y + (j + 1) * r, WOOD, True)
        vm = va + d * nsteps * tread
        self.box(u0, vm, u1, vf, y + S / 2 - 0.5, y + S / 2, WOOD, True)
        for j in range(nsteps): self.box(um, vm - d * j * tread, u1, vm - d * (j + 1) * tread, y + S / 2, y + S / 2 + (j + 1) * r, WOOD, True)


# --- hero spaces --------------------------------------------------------------------------------------------------------
def rail(m, x0, z0, x1, z1, y): m.det((x0, y, z0), (x1, y + 1.2, z1), STONE)


def hall(b, seg):
    m = b.m; F0, F1, F2 = Fy(0), Fy(1), Fy(2)
    for a, c in (((-28.5, -22.5), (28.5, -17.5)), ((-28.5, 17.5), (28.5, 22.5)), ((-28.5, -17.5), (-23.5, 17.5)), ((23.5, -17.5), (28.5, 17.5)), ((-23.5, -3), (23.5, 3))):
        m.det((a[0], F1 - 0.5, a[1]), (c[0], F1, c[1]), WOOD)
    m.stairs(-5, 5, 17.3, -1, F0, F1, riser=0.25, tread=0.55)
    for x0, x1 in ((-23.5, -5), (5, 23.5)): rail(m, x0, 2.8, x1, 3, F1); rail(m, x0, -3, x1, -2.8, F1)
    rail(m, -23.5, -17.5, 23.5, -17.3, F1)
    for a, c in (((-28.5, -22.5), (28.5, -18.5)), ((-28.5, 18.5), (28.5, 22.5)), ((-28.5, -18.5), (-24.5, 18.5)), ((24.5, -18.5), (28.5, 18.5))):
        m.det((a[0], F2 - 0.5, a[1]), (c[0], F2, c[1]), WOOD)
    for sx in (-1, 1):
        lo, hi = sorted((sx * 19.5, sx * 24.5)); m.det((lo, F1 + 3.0, -12), (hi, F1 + 3.5, -6), STONE)    # ledges +3.5 (F1 -> F2)
    prism(m, 0, -10, 4, F1 + 3.5, F1 + 4.0, 10, "dev/metal")                                              # chandelier


def library(b, seg):
    m = b.m; F0, F1 = Fy(0), Fy(1); u0 = seg[0] + TI; xw, xe = b.iv0, b.iv1
    m.det((xw, F1 - 0.5, u0), (xe, F1, u0 + 8), WOOD); rail(m, xw, u0 + 7.8, -133, u0 + 8, F1); rail(m, -127, u0 + 7.8, xe, u0 + 8, F1)
    for y in (6.0, 14.5): m.det((xw, y - 0.5, u0 + 10), (xw + 4, y, seg[1]), WOOD)                        # west tiers
    m.det((xe - 4, 10.0, u0 + 10), (xe, 10.5, seg[1]), WOOD)                                               # east tier
    for i, y in enumerate((6.0, 10.5, 14.5)): m.det((xw + 4, F0, u0 + 14 + 6 * i), (xw + 8, y, u0 + 20 + 6 * i), WOOD)  # book towers
    for x0 in (-137, -127):
        for z0 in (u0 + 14, u0 + 34, u0 + 54): m.det((x0, F0, z0), (x0 + 4, F0 + 3, z0 + 12), WOOD)


def ballroom(b, seg):
    m = b.m; F0, F1, F2 = Fy(0), Fy(1), Fy(2); u0 = seg[0] + TI; xw, xe = b.iv0, b.iv1
    for x in (122, 138):
        for z in (u0 + 15, u0 + 35, u0 + 55): prism(m, x, z, 1.0, F0, F2 - 0.5, 8, STONE)
    m.det((xw, F0, seg[1] - 12), (xe, F0 + 4, seg[1]), WOOD)                                               # stage (+4): the end
    m.det((xw, F1 - 0.5, u0), (xe, F1, u0 + 4), WOOD); rail(m, xw, u0 + 3.8, xe - 4.5, u0 + 4, F1)            # entry balcony
    m.det((xe - 4.5, F1 - 0.5, u0 + 4), (xe, F1, seg[1] - 12), WOOD); rail(m, xe - 4.9, u0 + 8, xe - 4.5, seg[1] - 12, F1)  # musicians' gallery


def kitchen(b, seg):
    m = b.m; F0 = Fy(0)
    m.det((b.iv0, F0, 185), (b.iv0 + 2, F0 + 5, 200), STONE)                                              # hearth
    for x in (-140, -122): m.det((x - 5, F0, 180), (x + 5, F0 + 1.1, 186), WOOD)


def chapel(b, seg):
    m = b.m; F0 = Fy(0)
    for z in range(178, 202, 4):
        for x0, x1 in ((112, 126), (134, 148)): m.det((x0, F0, z), (x1, F0 + 1.0, z + 1.2), WOOD)
    m.det((118, F0, 205), (142, F0 + 1.5, 212), STONE); prism(m, 130, 209, 1.2, F0 + 1.5, F0 + 4, 4, STONE)


HERO = {"hall": hall, "library": library, "ballroom": ballroom, "kitchen": kitchen, "chapel": chapel}


def mansard(x0, z0, x1, z1, h, inset):
    return lambda b: roof(b.m, x0 - 1, z0 - 1, x1 + 1, z1 + 1, Fy(b.n), h, inset)


def house(m):
    L = [0, 1, 2, 3]
    main = Bld(m, -150, -20, 150, 20, 5, "x", 0,
               {"N": {"spans": [(-150, -30), (30, 150)], "win": [(-150, -30), (30, 150)], "doors": [(80, [0], 5, 5)]},
                "S": {"spans": [(-150, -30), (30, 150)], "win": [(-110, -30), (30, 110)], "doors": [(-130, L, CW, 5), (130, L, CW, 5)]},
                "W": {"win": [(-20, 20)]}, "E": {"win": [(-20, 20)]}},
               [(-148.5, -30, "rooms", 0, 0), (30, 148.5, "rooms", 0, 0)],
               [{"u0": -100, "u1": -88, "side": "lo", "kind": "stair", "floors": None}, {"u0": 88, "u1": 100, "side": "lo", "kind": "stair", "floors": None},
                {"u0": 62, "u1": 86, "side": "lo", "kind": "garden_salon", "floors": [0]},
                {"u0": -100, "u1": -62, "side": "hi", "kind": "dining", "floors": [0]},
                {"u0": -136, "u1": -124, "side": "hi", "kind": "link", "floors": L}, {"u0": 124, "u1": 136, "side": "hi", "kind": "link", "floors": L}],
               seed=11, roofspec=mansard(-150, -20, 150, 20, 10, 4))
    pav = Bld(m, -30, -24, 30, 24, 5, "x", 0,
              {"N": {"win": [(-30, 30)]}, "S": {"win": [(-30, 30)], "doors": [(-12, [0], 5, 7), (0, [0], 5, 7), (12, [0], 5, 7)]},
               "W": {"doors": [(0, [0, 1, 2, 3, 4], CW, 5)]}, "E": {"doors": [(0, [0, 1, 2, 3, 4], CW, 5)]}},
              [(-28.5, 28.5, "hall", 0, 3)], seed=12,
              roofspec=lambda b: (roof(m, -31, -25, 31, 25, Fy(5), 16, 6), cone(m, 0, 0, 7, Fy(5) + 16, Fy(5) + 52, 4, ROOF, detail=False)))
    wings = []
    for sx, kind, fb, seed in ((-1, "library", 3, 13), (1, "ballroom", 2, 14)):
        x0, x1 = sorted((sx * 110, sx * 150)); cvx = sx * 130
        sides = {"N": {"own": False}, "S": {"own": False},
                 "W": {"win": [(20, 170)], "doors": ([(70, [0], 4, 5)] if sx < 0 else [(115, [0], 4, 5), (135, [0], 4, 5), (155, [0], 4, 5)])},
                 "E": {"win": [(20, 170)]}}
        lo_side = "lo" if sx < 0 else "hi"
        wings.append(Bld(m, x0, 20, x1, 170, 4, "z", cvx, sides, [(20, 95, "rooms", 0, 0), (95, 170, kind, 0, fb)],
                         [{"u0": 40, "u1": 52, "side": lo_side, "kind": "stair", "floors": None}], seed=seed,
                         roofspec=mansard(x0, 20, x1, 170, 9, 21),
                         xwall_floors={("library", 95): [2, 3], ("ballroom", 95): [2, 3]} if False else {("library", 95): [2], }))
    pavs = []
    for sx, kind, fb, seed in ((-1, "kitchen", 1, 15), (1, "chapel", 3, 16)):
        x0, x1 = sorted((sx * 104, sx * 156)); cvx = sx * 130
        sides = {"N": {"win": [], "doors": [(cvx, L, CW, 5)]}, "S": {"win": [(x0, x1)]}, "W": {"win": [(170, 215)]},
                 "E": {"win": [(170, 215)], "doors": ([(195, [0], 4, 5)] if sx < 0 else [])}}
        pavs.append(Bld(m, x0, 170, x1, 215, 4, "z", cvx, sides, [(171.5, 213.5, kind, 0, fb)], seed=seed,
                        roofspec=lambda b, x0=x0, x1=x1, sx=sx: (roof(m, x0 - 1, 169, x1 + 1, 216, Fy(4), 16, 22),
                                                                cone(m, sx * 130, 192.5, 2.5, Fy(4) + 16, Fy(4) + 34, 4, ROOF, detail=False))))
    for b in [main, pav] + wings + pavs: b.build()
    # plinth / cornice bands on the main facades (articulation), entrance terrace and steps
    for x0, z0, x1, z1, H in ((-150, -20, 150, 20, Fy(5)), (-30, -24, 30, 24, Fy(5))):
        for y0, y1, o in ((0, FL + 0.3, 0.6), (H - 1.2, H, 1.0)):
            for a, c in (((x0 - o, y0, z0 - o), (x1 + o, y1, z0)), ((x0 - o, y0, z1), (x1 + o, y1, z1 + o))): m.det(a, c, STONE)
    m.det((-24, 0, 24), (24, FL, 34), STONE); m.stairs(-16, 16, 37.5, -1, 0, FL, riser=0.25, tread=0.6)


def hedge(m, x0, z0, x1, z1, h=7.5): m.mass((x0, 0, z0), (x1, h, z1), HEDGE)
def low(m, x0, z0, x1, z1): m.det((x0, 0, z0), (x1, 1.2, z1), HEDGE)
def statue(m, x, z): m.det((x - 1.5, 0, z - 1.5), (x + 1.5, 2.5, z + 1.5), STONE); prism(m, x, z, 0.6, 2.5, 5.5, 4, STONE)
def tree(m, x, z): prism(m, x, z, 0.5, 0, 4, 6, WOOD); cone(m, x, z, 3.2, 4, 12, 8, HEDGE)


def fountain(m, x, z, r):
    prism(m, x, z, r, 0, 1.2, 16, STONE); prism(m, x, z, r * 0.28, 1.2, 4.5, 8, STONE)
    prism(m, x, z, r * 0.55, 4.5, 5.0, 12, STONE); prism(m, x, z, r * 0.07, 5.0, 9, 6, STONE)


def grounds(m):
    m.mass((BOUNDS[0], -0.5, BOUNDS[2]), (BOUNDS[1], 0, BOUNDS[3]), {"side": LAWN, "top": LAWN, "bottom": LAWN})
    # COURT between the wings: paving, fountain, low parterres, statues, fence + gate
    m.det((-110, 0, 20), (110, 0.05, 215), PATH); fountain(m, 0, 120, 10)
    for sx in (-1, 1):
        for z0 in (60, 150): low(m, *sorted((sx * 40, sx * 80))[:1], z0, *sorted((sx * 40, sx * 80))[1:], z0 + 30)
        statue(m, sx * 20, 60)
    for x0, x1 in ((-104, -8), (8, 104)): m.mass((x0, 0, 215), (x1, 1.2, 216), STONE); m.det((x0, 1.2, 215.4), (x1, 8, 215.6), "dev/grate")
    for x in (-104, -78, -52, -26, -8, 8, 26, 52, 78, 104): m.mass((x - 0.8, 0, 214.7), (x + 0.8, 8.5, 216.3), STONE)
    # FRONT: parterre, round point, allee with bosquets, gate, start enclosure; strip hedges x +-(104..107)
    m.det((-7, 0, 216.3), (7, 0.05, 650), PATH)
    hedge(m, -107, 215, -104, 260); hedge(m, -107, 276, -104, 653); hedge(m, 104, 215, 107, 653)
    for sx in (-1, 1):
        for z0 in (230, 280):
            lo_, hi_ = sorted((sx * 15, sx * 45)); low(m, lo_, z0, hi_, z0 + 30)
            lo_, hi_ = sorted((sx * 55, sx * 90)); low(m, lo_, z0, hi_, z0 + 30)
        statue(m, sx * 50, 320)
    for a, b_, c, d in ((-58, 335, -7, 338), (7, 335, 58, 338), (-58, 442, -7, 445), (7, 442, 58, 445), (-58, 338, -55, 442), (55, 338, 58, 442)):
        hedge(m, a, b_, c, d)
    fountain(m, 0, 390, 14)
    for x, z in ((-35, 360), (35, 360), (-35, 420), (35, 420)): statue(m, x, z)
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 22, sx * 25))
        hedge(m, x0, 445, x1, 500); hedge(m, x0, 512, x1, 590); hedge(m, x0, 602, x1, 653)
        for z in range(452, 650, 14): tree(m, sx * 13, z)
        bx0, bx1 = sorted((sx * 25, sx * 93))
        for z0, z1 in ((467, 543), (557, 633)):
            hedge(m, bx0, z0, bx1, z0 + 3); hedge(m, bx0, z1 - 3, bx1, z1)
            ox0, ox1 = sorted((sx * 90, sx * 93)); hedge(m, ox0, z0, ox1, z1)
        prism(m, sx * 58, 505, 2, 0, 14, 4, STONE)                       # bosquet features: obelisk / kiosk
        prism(m, sx * 58, 595, 7, 0, 2.0, 8, STONE); cone(m, sx * 58, 595, 7.5, 7, 10, 8, ROOF)
        for i in range(8): prism(m, sx * 58 + 6 * math.cos(i * math.pi / 4), 595 + 6 * math.sin(i * math.pi / 4), 0.35, 2.0, 7, 6, WALL)
    hedge(m, BOUNDS[0], 650, -8, 653, 8); hedge(m, 8, 650, BOUNDS[1], 653, 8)
    for x in (-9.5, 9.5): m.mass((x - 1.5, 0, 649), (x + 1.5, 10, 654), STONE)
    hedge(m, -33, 680, 33, 683, 8); hedge(m, -33, 653, -30, 680, 8); hedge(m, 30, 653, 33, 680, 8)
    # WEST: bosquet garden with a gazebo, service yard, the west walk (alcoves, pergola, fight pocket)
    hedge(m, -308, 17, -305, 653, 8)
    hedge(m, -305, 400, -107, 403); hedge(m, -305, 227, -260, 230); hedge(m, -230, 227, -107, 230)
    prism(m, -200, 315, 14, 0, 2.0, 8, STONE)
    for i in range(8):
        a = math.pi / 8 + i * math.pi / 4; prism(m, -200 + 12.5 * math.cos(a), 315 + 12.5 * math.sin(a), 0.6, 2.0, 9, 6, WALL)
    cone(m, -200, 315, 16, 9, 15, 8, ROOF)
    for z0 in (250, 360): low(m, -280, z0, -240, z0 + 25); low(m, -160, z0, -120, z0 + 25)
    for z0 in (60, 90, 120, 150): low(m, -290, z0, -200, z0 + 8)                                     # kitchen garden beds
    hedge(m, -308, 14, -240, 17, 8); hedge(m, -243, -350, -240, 17, 8)
    for z0, z1 in ((-8, 17), (-48, -20), (-88, -60), (-170, -100)): hedge(m, -240, z0, -226, z1)
    for z in (-14, -54, -94): statue(m, -233, z)
    hedge(m, -186, -100, -153, -20)
    for z in range(10, -100, -10):
        for x in (-220, -192): prism(m, x, z, 0.6, 0, 10.4, 6, WOOD)
    for z in range(12, -98, -4): m.det((-221, 10.4, z - 0.3), (-191, 11.0, z + 0.3), WOOD)
    fountain(m, -190, -135, 8); prism(m, -170, -115, 2, 0, 18, 4, STONE)
    m.det((-210, 0, -160), (-204, 3, -154), HEDGE); m.det((-178, 0, -158), (-172, 3, -152), HEDGE)
    hedge(m, -226, -200, -155, -170); hedge(m, -243, -203, -150, -200)
    # BACK: topiary court -> rose garden (red key); conservatory (red lock) -> garden salon
    hedge(m, -243, -353, 136, -350, 8); hedge(m, 133, -350, 136, -20, 8)
    hedge(m, -153, -200, -150, -160); hedge(m, -153, -130, -150, -20)
    hedge(m, -150, -100, 85, -97); hedge(m, 105, -100, 133, -97); hedge(m, 57, -97, 60, -80)
    hedge(m, -150, -193, -10, -190); hedge(m, 10, -193, 133, -190)
    for x in range(-130, 121, 25): tree(m, x, -170); tree(m, x + 12, -120)
    hedge(m, -73, -350, -70, -193); hedge(m, 70, -350, 73, -193)
    prism(m, 0, -260, 16, 0, 2.0, 12, LAWN); prism(m, 0, -260, 1.5, 2.0, 6.0, 6, STONE)
    for x, z in ((-40, -230), (40, -230), (-40, -300), (40, -300)): low(m, x - 8, z - 5, x + 8, z + 5)
    m.det((60, 0, -80), (120, FL, -20), STONE)
    for a, c in (((60, -80), (60.4, -20)), ((119.6, -80), (120, -20)), ((60, -80), (83, -79.6)), ((97, -80), (120, -79.6))):
        m.det((a[0], FL, a[1]), (c[0], FL + 16, c[1]), GLASS)
    roof(m, 60, -80, 120, -20, FL + 16, 8, 30, GLASS)
    for x, z in ((72, -68), (108, -68), (90, -50), (72, -32), (108, -32)): m.det((x - 4, FL, z - 4), (x + 4, FL + 3, z + 4), LAWN)


def gameplay(m):
    F0, F1 = Fy(0), Fy(1)
    m.door("conservatory_door", ((83, FL, -79.9), (97, FL + 16, -79.7)), GLASS)
    m.brush_ent("trigger_key_once", [((81, 0, -92), (99, 8, -70))], requiredKeys="red", target="conservatory_door")
    for name, y in (("ballroom_door", F0), ("ballroom_balcony_door", F1)):
        m.door(name, ((127, y, 95.3), (133, y + 5, 95.7)))
        m.brush_ent("trigger_key_once", [((125, y, 88), (135, y + 5, 103))], requiredKeys="blue", target=name)
    for z in (115, 135, 155): m.door(f"ballroom_french_{z}", ((110.5, F0, z - 2), (110.9, F0 + 5, z + 2)))
    m.door("service_door", ((-149.5, F0, 68), (-149.1, F0 + 5, 72)))
    m.det((-156, 0, 64), (-150, FL, 76), STONE)
    m.brush_ent("triggerOnce", [((-148, F0, 64), (-140, F0 + 4, 76))], target="service_door")
    m.door("court_gate", ((-8, 0, 215.4), (8, 8, 215.6)), "dev/grate")
    m.brush_ent("triggerOnce", [((-12, 0, 200), (12, 5, 214))], target="court_gate")
    m.brush_ent("triggerOnce", [((112, F0 + 4, 158), (148, F0 + 9, 170))], target="level_end")
    m.point("logic_level_end", (130, F0 + 5, 164), targetname="level_end", nextLevel="")


AXE, GUN, DOG = "npc_human_axe", "npc_human_gun", "npc_dog"
ITEMS = [("item_weapon_twinswords", 0, 662, 0), ("item_key_red", 0, -260, 6), ("item_key_blue", -146.5, 165, 14.5),
         ("item_key_secret", -26, -20.5, Fy(2))]
ENEMIES = {
    "allee": [(AXE, -4, 600, 0), (AXE, 4, 560, 0), (DOG, 0, 520, 0)],
    "bosquets": [(AXE, -60, 490, 0), (AXE, -50, 525, 0), (GUN, -58, 595, 2), (AXE, 55, 580, 0), (AXE, 65, 610, 0)],
    "round point": [(AXE, -25, 370, 0), (AXE, 25, 370, 0), (AXE, -25, 410, 0), (AXE, 25, 410, 0), (DOG, -10, 350, 0), (DOG, 10, 350, 0), (GUN, 4, 390, 5)],
    "parterre": [(AXE, -30, 300, 0), (AXE, 30, 260, 0), (DOG, 0, 240, 0)],
    "gazebo garden": [(AXE, -260, 300, 0), (AXE, -140, 300, 0), (AXE, -250, 380, 0), (GUN, -200, 305, 2), (GUN, -200, 325, 2), (DOG, -150, 380, 0)],
    "service yard": [(DOG, -250, 100, 0), (DOG, -230, 180, 0), (AXE, -180, 60, 0)],
    "west walk": [(AXE, -206, -10, 0), (AXE, -206, -70, 0)],
    "fight pocket": [(AXE, -215, -120, 0), (AXE, -165, -150, 0), (AXE, -215, -160, 0), (GUN, -190, -128, 1.2)],
    "topiary court": [(AXE, -80, -150, 0), (AXE, 0, -140, 0), (AXE, 80, -160, 0), (DOG, 40, -130, 0)],
    "rose garden": [(DOG, -40, -215, 0), (DOG, 40, -215, 0), (DOG, -50, -320, 0), (DOG, 50, -320, 0), (AXE, 0, -210, 0), (AXE, -25, -290, 0), (AXE, 25, -290, 0)],
    "conservatory": [(AXE, 80, -70, FL), (AXE, 100, -45, FL), (GUN, 72, -68, FL + 3), (GUN, 108, -68, FL + 3)],
    "garden salon": [(AXE, 70, -12, FL), (AXE, 80, -14, FL)],
    "main corridor": [(AXE, 50, 0, FL), (AXE, -60, 0, FL)],
    "entrance hall": [(AXE, -15, 10, FL), (AXE, 15, 10, FL), (AXE, -15, -12, FL), (AXE, 15, -12, FL), (GUN, -26, 0, Fy(1)), (GUN, 26, 0, Fy(1)), (GUN, 0, -20, Fy(1))],
    "dining": [(AXE, -85, 12, FL), (AXE, -75, 14, FL)],
    "west wing": [(AXE, -130, 60, FL), (DOG, -130, 80, FL), (AXE, -130, 40, Fy(1))],
    "library": [(AXE, -130, 120, FL), (AXE, -120, 150, FL), (AXE, -140, 140, FL), (GUN, -146.5, 130, 6), (GUN, -113.5, 140, 10.5), (GUN, -146.5, 150, 14.5)],
    "kitchen": [(DOG, -140, 195, FL), (DOG, -120, 200, FL), (AXE, -130, 190, FL)],
    "court": [(AXE, -40, 110, 0), (AXE, 40, 110, 0), (DOG, 0, 160, 0)],
    "upper floors": [(AXE, -80, 0, Fy(2)), (AXE, 80, 0, Fy(3)), (DOG, 0, 0, Fy(4))],
    "ballroom (boss)": [(AXE, 120, 110, FL), (AXE, 140, 110, FL), (AXE, 120, 140, FL), (AXE, 140, 140, FL), (AXE, 130, 125, FL),
                        (GUN, 146, 120, Fy(1)), (GUN, 146, 145, Fy(1)), (DOG, 118, 152, FL), (DOG, 142, 152, FL)],
}
CHECKS = [("start", 0, 668, 0), ("round point", 0, 360, 0), ("gazebo garden", -260, 300, 0), ("fight pocket", -215, -120, 0),
          ("red key", 0, -260, 6), ("conservatory", 80, -70, FL), ("garden salon", 70, -12, FL), ("entrance hall", 15, 10, FL),
          ("F1 bridge", 15, 0, Fy(1)), ("library", -130, 120, FL), ("blue key", -146.5, 165, 14.5), ("secret key", -26, -20.5, Fy(2)),
          ("kitchen", -130, 195, FL), ("court", 40, 110, 0), ("room F2", -80, -12, Fy(2)), ("room F3", 80, 12, Fy(3)), ("attic F4", -80, 0, Fy(4)), ("wing F3", -130, 60, Fy(3)), ("ballroom", 130, 125, FL), ("END", 130, 164, FL + 4)]
STATES = [("no keys", ()), ("red used", ("conservatory_door",)), ("red + blue", ("conservatory_door", "ballroom_door", "ballroom_balcony_door"))]


def build():
    m = Level(); grounds(m); house(m); gameplay(m)
    reach.CELL, reach.TILE = 1.0, 8.0
    snap = reach.Model(m.doc(), open_doors=("conservatory_door", "ballroom_door", "ballroom_balcony_door", "court_gate"), bounds=BOUNDS)
    for cls, x, z, y in ITEMS: m.point(cls, (x, snap.floor_at(x, z, y) + 0.5, z))
    for lst in ENEMIES.values():
        for cls, x, z, y in lst: m.point(cls, (x, snap.floor_at(x, z, y) + 1.0, z), angle=270)
    m.point("info_player_start", (0, 0.1, 668), angle=90)
    return m


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    m = build(); d = m.doc()
    json.dump(d, open(os.path.join(OUT, "mansion_big.map.json"), "w"))
    print("brushes", sum(len(e.get("brushes", [])) for e in d["entities"]), "entities", len(d["entities"]))
    for name, od in STATES:
        md = reach.Model(d, open_doors=od, bounds=BOUNDS); seen = md.reach(0, 668, 0)
        print(f"[{name}] cells {len(seen)}: " + "  ".join(f"{l}={'Y' if md.at(seen, x, z, y) else '-'}" for l, x, z, y in CHECKS))
