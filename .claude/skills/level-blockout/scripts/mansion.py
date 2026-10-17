"""Roswaal-style mansion for a slasher. Player: fast, jumps ~5.4 m (3x height), hovers, dashes; far third-person camera.
So: big interiors (16 m halls, 6-8 m corridors, 5 x 6.5 m doors), boundaries >= 8 m (unjumpable), low things (1-5 m) are platforms.
Garden = hedge rooms, never an open lawn. A = front approach (iconic view first). B = back approach via hedge rooms + conservatory,
iconic front view revealed from the balcony, finale in the front garden. Meters, Y up, north = -Z, mansion faces south (+Z)."""
import json, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapgen import Map, OUT

WALL, ROOF, PATH, LAWN, HEDGE, STONE, GLASS = "dev/grey", "dev/metal", "dev/concrete", "dev/floor", "dev/floor", "dev/concrete", "dev/glass"
FL = 1.5  # ground floor level (the house stands on a plinth)


def sub(a, b): return [a[i] - b[i] for i in range(3)]
def dot(a, b): return sum(a[i] * b[i] for i in range(3))
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def T(t): return t if isinstance(t, dict) else {"side": t, "top": t, "bottom": t}


def hull(m, pts, tex, detail=False):
    """Convex brush from points: one face per supporting plane, loop CCW seen from outside."""
    P, faces, seen, n = [list(map(float, p)) for p in pts], [], set(), len(pts)
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                nr = cross(sub(P[j], P[i]), sub(P[k], P[i])); L = dot(nr, nr) ** 0.5
                if L < 1e-9: continue
                nr = [x / L for x in nr]; s = [dot(nr, p) - dot(nr, P[i]) for p in P]
                if all(x >= -1e-6 for x in s): nr, s = [-x for x in nr], [-x for x in s]
                elif not all(x <= 1e-6 for x in s): continue
                on = tuple(q for q in range(n) if abs(s[q]) < 1e-6)
                if on in seen: continue
                seen.add(on)
                c = [sum(P[q][a] for q in on) / len(on) for a in range(3)]
                u = sub(P[on[0]], c); u = [x / dot(u, u) ** 0.5 for x in u]; w = cross(nr, u)
                loop = sorted(on, key=lambda q: math.atan2(dot(sub(P[q], c), w), dot(sub(P[q], c), u)))
                t = T(tex); faces.append({"id": len(faces), "loop": loop, "texture": t["top"] if nr[1] > 0.7 else t["bottom"] if nr[1] < -0.7 else t["side"]})
    m.n += 1
    (m.detail if detail else m.world).append({"id": f"b{m.n}", "op": "add", "vertices": P, "faces": faces})


def roof(m, x0, z0, x1, z1, y, h, inset, tex=ROOF):
    """Mansard / hip roof: base rectangle up to an inset rectangle, a ridge when the inset reaches the middle."""
    hw, hd = (x1 - x0) / 2, (z1 - z0) / 2
    base = [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]
    if inset >= min(hw, hd) - 1e-3:
        r = min(hw, hd)
        top = [(x0 + hw, y + h, z0 + hd)] if abs(hw - hd) < 1e-3 else \
              [(x0 + r, y + h, z0 + hd), (x1 - r, y + h, z0 + hd)] if hw > hd else [(x0 + hw, y + h, z0 + r), (x0 + hw, y + h, z1 - r)]
    else:
        top = [(x0 + inset, y + h, z0 + inset), (x1 - inset, y + h, z0 + inset), (x1 - inset, y + h, z1 - inset), (x0 + inset, y + h, z1 - inset)]
    hull(m, base + top, tex)


def ngon(cx, cz, r, y, sides): return [(cx + r * math.cos(math.pi / sides + 2 * math.pi * i / sides), y, cz + r * math.sin(math.pi / sides + 2 * math.pi * i / sides)) for i in range(sides)]
def prism(m, cx, cz, r, y0, y1, sides, tex, detail=True): hull(m, ngon(cx, cz, r, y0, sides) + ngon(cx, cz, r, y1, sides), tex, detail)
def cone(m, cx, cz, r, y0, y1, sides, tex, detail=True): hull(m, ngon(cx, cz, r, y0, sides) + [(cx, y1, cz)], tex, detail)
def hedge(m, x0, z0, x1, z1, h=8.5): m.mass((x0, 0, z0), (x1, h, z1), HEDGE)
def topiary(m, x, z, h=8.0): m.det((x - 1.4, 0, z - 1.4), (x + 1.4, 1.2, z + 1.4), STONE); cone(m, x, z, 2.4, 1.2, h, 8, HEDGE)


def block(m, x0, z0, x1, z1, storeys, roof_h, inset, faces="", skip=(), bay=6.0):
    """Palace volume: solid mass, plinth, string courses, cornice, roof, a 2.4 m wide window per bay and storey (2.1 m deep)."""
    H = FL + sum(storeys)
    m.mass((x0, -0.3, z0), (x1, H, z1), {"side": WALL, "top": ROOF, "bottom": WALL})
    def ring(y0, y1, o):
        for a, b in (((x0 - o, y0, z0 - o), (x1 + o, y1, z0)), ((x0 - o, y0, z1), (x1 + o, y1, z1 + o)),
                     ((x0 - o, y0, z0), (x0, y1, z1)), ((x1, y0, z0), (x1 + o, y1, z1))):
            m.det(a, b, STONE)
    ring(0, FL + 0.4, 0.5)
    y = FL
    for s in storeys[:-1]:
        y += s; ring(y - 0.3, y + 0.25, 0.35)
    ring(H - 1.3, H, 1.1)
    roof(m, x0 - 0.8, z0 - 0.8, x1 + 0.8, z1 + 0.8, H, roof_h, inset)
    win = [(1.6, 5.0), (1.2, 5.4), (1.0, 3.6), (1.0, 3.4)]
    for f in faces:
        a0, a1 = (x0, x1) if f in "NS" else (z0, z1)
        n = int((a1 - a0) // bay); pad = (a1 - a0 - n * bay) / 2
        for i in range(n):
            c = a0 + pad + bay * (i + 0.5)
            if any(g == f and lo <= c <= hi for g, lo, hi in skip): continue
            yb = FL
            for k, s in enumerate(storeys):
                sill, wh = win[min(k, 3)]; ya, yz = yb + sill, yb + sill + min(wh, s - sill - 0.8)
                if f == "S": m.cut((c - 1.2, ya, z1 - 2.1), (c + 1.2, yz, z1 + 0.1))
                if f == "N": m.cut((c - 1.2, ya, z0 - 0.1), (c + 1.2, yz, z0 + 2.1))
                if f == "E": m.cut((x1 - 2.1, ya, c - 1.2), (x1 + 0.1, yz, c + 1.2))
                if f == "W": m.cut((x0 - 0.1, ya, c - 1.2), (x0 + 2.1, yz, c + 1.2))
                yb += s


def rail(m, x0, z0, x1, z1, y): m.det((x0, y, z0), (x1, y + 1.2, z1), STONE)


def interior(m, G):
    """Ground floor FL, first floor G (= FL + 8). Doors 5 x 6.5 m with frames, pilasters every 6 m."""
    WOOD = "dev/wood"
    def pil_z(x0, x1, z0, z1, y0, y1):          # pilasters on a wall facing -z / +z (z0..z1 = their depth)
        x = x0 + 3
        while x < x1: m.det((x - 0.5, y0, z0), (x + 0.5, y1, z1), STONE); x += 6
    def door_z(x, z0, z1, y, w=5.0, h=6.5):     # through a wall spanning z0..z1, framed on both faces
        m.cut((x - w / 2, y, z0 - 0.1), (x + w / 2, y + h, z1 + 0.1))
        for a, b in ((z0 - 0.4, z0), (z1, z1 + 0.4)):
            m.det((x - w / 2 - 0.7, y, a), (x - w / 2, y + h + 0.7, b), STONE); m.det((x + w / 2, y, a), (x + w / 2 + 0.7, y + h + 0.7, b), STONE)
            m.det((x - w / 2, y + h, a), (x + w / 2, y + h + 0.7, b), STONE)
    def door_x(z, x0, x1, y, w=5.0, h=6.5):
        m.cut((x0 - 0.1, y, z - w / 2), (x1 + 0.1, y + h, z + w / 2))
        for a, b in ((x0 - 0.4, x0), (x1, x1 + 0.4)):
            m.det((a, y, z - w / 2 - 0.7), (b, y + h + 0.7, z - w / 2), STONE); m.det((a, y, z + w / 2), (b, y + h + 0.7, z + w / 2 + 0.7), STONE)
            m.det((a, y + h, z - w / 2), (b, y + h + 0.7, z + w / 2), STONE)
    # entrance hall: rises another 6 m at the front; the master's loggia sits above the back gallery, reached by wall ledges
    m.cut((-12, FL + 16, -5), (12, FL + 22, 13)); m.cut((-12, FL + 16, -13), (12, FL + 22, -5))
    m.det((-12, FL + 15.6, -13), (12, FL + 16, -5), WOOD); rail(m, -12, -5.2, -2, -5, FL + 16); rail(m, 2, -5.2, 12, -5, FL + 16)
    for sx in (-1, 1): m.det((min(sx * 12, sx * 10), FL + 11.6, -3), (max(sx * 12, sx * 10), FL + 12, 3), STONE)  # ledges at +12
    prism(m, 0, 4, 2.5, FL + 13, FL + 13.6, 8, "dev/metal")                                    # chandelier (a platform)
    m.det((-3, FL + 16, -12), (3, FL + 17.2, -10), WOOD)                                       # the master's desk
    # dining hall (west, 32 x 20): long table, fireplace, pilasters
    m.cut((-46, FL, -10), (-14, FL + 8, 10)); door_x(0, -14, -12, FL)
    m.det((-38, FL, -2), (-20, FL + 1.1, 2), WOOD)
    m.cut((-47.6, FL, -3), (-45.9, FL + 3.5, 3)); m.det((-46, FL + 3.5, -4), (-45.4, FL + 4.1, 4), STONE)
    pil_z(-46, -14, -10, -9.5, FL, FL + 8)
    # music salon (east, 32 x 20): musicians' dais
    m.cut((14, FL, -10), (46, FL + 8, 10)); door_x(0, 12, 14, FL)
    m.det((36, FL, -10), (46, FL + 1.2, -2), WOOD); pil_z(14, 46, -10, -9.5, FL, FL + 8)
    # first floor: corridor of doors (8 x 7), guest rooms + study on its north side
    m.cut((-46, G, 2), (46, G + 7, 10))
    for x0, x1 in ((-46, -12), (12, 46)): pil_z(x0, x1, 2, 2.5, G, G + 7); pil_z(x0, x1, 9.5, 10, G, G + 7)
    for x0, x1, kind in ((-46, -34, "bed"), (-32, -20, "bed"), (14, 28, "study"), (30, 46, "bed")):
        m.cut((x0, G, -10), (x1, G + 7, 0)); door_z((x0 + x1) / 2, 0, 2, G, w=4, h=6)
        if kind == "bed": m.det((x0 + 2, G, -10), (x0 + 6, G + 1.2, -5), WOOD)
        else: m.det((x0, G, -10), (x1, G + 5, -9), WOOD); m.det((x0 + 5, G, -6), (x0 + 9, G + 1.2, -4), WOOD)
    # the Forbidden Library: whole west wing, 17 x 44 x 16; bookcase ledges at +5 / +10 on both long walls, book stacks
    # (4.5 m platforms), a mezzanine at +8 entered from the corridor of doors
    m.cut((-46, FL, 14), (-29, FL + 16, 58), floor=WOOD, wall=WOOD)
    door_z(-37.5, 10, 14, FL); door_z(-37.5, 10, 14, G)
    for x0, x1 in ((-46, -43.5), (-31.5, -29)):
        for y in (FL + 5, FL + 10): m.det((x0, y - 0.4, 20), (x1, y, 56), WOOD)
    for z in (26, 38, 50):
        for x in (-41, -34): m.det((x - 1, FL, z - 4), (x + 1, FL + 4.5, z + 4), WOOD)
    m.det((-46, G - 0.4, 14), (-29, G, 18), WOOD); rail(m, -46, 17.8, -40, 18, G); rail(m, -35, 17.8, -29, 18, G)
    # kitchen: west end pavilion, hearth, tables, back door to the courtyard
    m.cut((-49, FL, 62), (-26, FL + 8, 76)); door_z(-37.5, 58, 62, FL); door_x(69, -26, -24, FL)
    m.cut((-50.6, FL, 66), (-48.9, FL + 4, 72))
    for x in (-42, -33): m.det((x - 2, FL, 66), (x + 2, FL + 1.1, 72), WOOD)
    # ballroom: east wing 17 x 44 x 16, french doors to the court, two columns, musicians' gallery at +8 from the corridor
    m.cut((29, FL, 14), (46, FL + 16, 58), floor="dev/floor"); door_z(37.5, 10, 14, FL)
    for z in (22, 36, 50): m.cut((26.9, FL, z - 2.2), (29.1, FL + 7, z + 2.2))
    for z in (25, 47): prism(m, 37.5, z, 1.0, FL, FL + 16, 8, STONE)
    m.det((41, G - 0.4, 14), (46, G, 58), WOOD); m.det((40.6, G, 18), (41, G + 1.2, 58), STONE); door_z(43.5, 10, 14, G)


def mansion(m, balcony):
    block(m, -48, -12, 48, 12, [8, 8, 6], 10, 3.5, faces="NS", skip=[("S", -15, 15), ("S", -48, -26), ("S", 26, 48)])
    block(m, -14, -15, 14, 15, [8, 8, 6, 6], 8, 4, faces="S", skip=[("S", -10, 10)])
    cone(m, 0, 0, 4.5, FL + 28 + 8, FL + 28 + 26, 4, ROOF, detail=False)
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 27, sx * 48))
        block(m, x0, 12, x1, 60, [8, 8], 8, 10.5, faces="EW")
        x0, x1 = sorted((sx * 24, sx * 51))
        block(m, x0, 60, x1, 78, [8, 8, 6], 12, 9, faces="SEW")
        cone(m, sx * 37.5, 69, 1.6, FL + 22 + 12, FL + 22 + 22, 4, ROOF, detail=False)
    # entrance: terrace + broad steps, three 4.4 x 7 m doors in the pavilion front
    m.det((-16, 0, 15), (16, FL, 21), STONE); m.stairs(-10, 10, 24, -1, 0, FL, riser=0.3, tread=0.6)
    for x in (-7, 0, 7): m.cut((x - 2.2, FL, 12.9), (x + 2.2, FL + 7, 15.1))
    # entrance hall 24 x 26 x 16 with an 8 m grand stair to a ring gallery at +8 m
    G = FL + 8
    m.cut((-12, FL, -13), (12, FL + 16, 13), floor="dev/floor")
    for a, b in (((-12, -13), (12, -5)), ((-12, -5), (-8, 13)), ((8, -5), (12, 13)), ((-8, 9), (8, 13))):
        m.det((a[0], G - 0.4, a[1]), (b[0], G, b[1]), "dev/wood")
    rail(m, -8, -5.2, -4, -5, G); rail(m, 4, -5.2, 8, -5, G); rail(m, -8.2, -5, -8, 9, G); rail(m, 8, -5, 8.2, 9, G); rail(m, -8, 9, 8, 9.2, G)
    m.stairs(-4, 4, 9, -1, FL, G, riser=0.3, tread=0.52)
    interior(m, G)
    if balcony:  # first-floor balcony over the doors: the reveal of the front garden
        m.cut((-2.5, G, 12.9), (2.5, G + 6.5, 15.1))
        m.det((-10, G - 0.4, 15), (10, G, 19.5), STONE); rail(m, -10, 19.3, 10, 19.5, G)


def front(m, gate_open):
    m.mass((-90, -0.3, -135), (90, 0, 200), {"side": LAWN, "top": LAWN, "bottom": LAWN})
    m.det((-27, 0, 12), (27, 0.05, 78), PATH)                                                  # cour d'honneur
    prism(m, 0, 45, 6, 0, 1.2, 12, STONE); prism(m, 0, 45, 1.2, 1.2, 6, 8, STONE)
    for x0, x1 in ((-24, -6), (6, 24)): m.mass((x0, 0, 78), (x1, 1.5, 79), STONE); m.det((x0, 1.5, 78.4), (x1, 9, 78.6), "dev/grate")
    for x in (-24, -6, 6, 24): m.mass((x - 0.8, 0, 77.7), (x + 0.8, 10, 79.3), STONE)
    if not gate_open: m.det((-6, 0, 78.4), (6, 9, 78.6), "dev/grate")
    # play strip 64 m wide between 8.5 m hedges; avenue; rows of 8 m topiaries; statue plinths as jump platforms
    m.det((-7.5, 0, 79.3), (7.5, 0.05, 190), PATH)
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 32, sx * 34))
        hedge(m, x0, 78, x1, 134); hedge(m, x0, 146, x1, 188)
        for z in (90, 102, 172, 182): topiary(m, sx * 12, z)
        for z in (96, 177): m.det((sx * 22 - 2, 0, z - 2), (sx * 22 + 2, 4, z + 2), STONE); prism(m, sx * 22, z, 0.8, 4, 7, 4, STONE)
    # fountain plaza 48 x 48 (arena): hedge ring with N/S/E/W gaps; tiered fountain = platforms at 1.2 and 5 m
    for a, b, c, d in ((-24, 116, -7.5, 118), (7.5, 116, 24, 118), (-24, 162, -7.5, 164), (7.5, 162, 24, 164),
                       (-24, 118, -22, 134), (-24, 146, -22, 162), (22, 118, 24, 134), (22, 146, 24, 162)):
        hedge(m, a, b, c, d)
    prism(m, 0, 140, 8, 0, 1.2, 16, STONE); prism(m, 0, 140, 2, 1.2, 4.5, 8, STONE)
    prism(m, 0, 140, 4.5, 4.5, 5.0, 12, STONE); prism(m, 0, 140, 0.8, 5.0, 9, 6, STONE)
    for x0, x1 in ((-90, -7.5), (7.5, 90)): hedge(m, x0, 188, x1, 190, 10)                     # front boundary + gate piers
    for x in (-8.5, 8.5): m.mass((x - 1, 0, 187.5), (x + 1, 11, 190.5), STONE)


def variant_a():
    m = Map(); front(m, gate_open=False); mansion(m, balcony=False)
    # west: plaza W gap -> gazebo garden (arena 2) -> north gap -> service yard along the west wing -> service door
    hedge(m, -82, 116, -34, 118); m.mass((-82, 0, 162), (-34, 8.5, 164), HEDGE)
    for a, b in ((-82, -76), (-64, -34)): pass
    hedge(m, -82, 10, -80, 188, 10)                                                            # west boundary
    m.world[:] = [b for b in m.world if not (b["vertices"][0][2] == 116 and b["vertices"][0][0] == -82)]  # reopen: rebuilt with a gap below
    hedge(m, -80, 116, -76, 118); hedge(m, -64, 116, -34, 118)
    prism(m, -58, 140, 7, 0, 1.5, 8, STONE); m.stairs(-60, -56, 149, -1, 0, 1.5, riser=0.3, tread=0.6)
    for i in range(8):
        a = math.pi / 8 + i * math.pi / 4
        prism(m, -58 + 6.2 * math.cos(a), 140 + 6.2 * math.sin(a), 0.4, 1.5, 9, 6, WALL)
    cone(m, -58, 140, 8.5, 9, 14, 8, ROOF)
    m.cut((-48.1, FL, 40), (-45.9, FL + 6.5, 45)); m.det((-52, 0, 38), (-48, FL, 47), STONE)  # service door + stoop
    back(m, west_entry=True)
    m.point("info_player_start", (0, 0.1, 196), angle=90)
    return m


def variant_b():
    m = Map(); front(m, gate_open=True); mansion(m, balcony=True)
    # back: gate -> rose room (arena 1) -> topiary court (arena 2) -> conservatory (arena 3) -> east salon -> hall -> balcony reveal
    back(m, west_entry=False)
    m.point("info_player_start", (0, 0.1, -122), angle=270)
    return m


def back(m, west_entry):
    """Back gardens: rose room -> topiary court -> conservatory -> east salon. west_entry: reached by the west walk around the
    main block's west end (from the service yard) through a 10 m gap in the topiary court's west hedge."""
    if west_entry:
        hedge(m, -68, -128, 48, -126, 10)
        hedge(m, -42, -128, -40, -66, 10); hedge(m, -42, -56, -40, -12, 10)                    # topiary court W wall, gap z -66..-56
        hedge(m, -68, -126, -66, 8, 10); hedge(m, -82, 8, -66, 10, 10)                       # west walk: x -66..-42, from the yard
        hedge(m, -66, -82, -42, -80, 10)                                                       # walk ends at the topiary court
        # the walk as a 10 m garden path (x -60..-50): statue alcoves in the west hedge, a pergola, then a fight pocket
        # (18 x 26 m) that guards the gap into the topiary court
        for z0, z1 in ((-6, 8), (-20, -12), (-48, -26), (-80, -54)): hedge(m, -66, z0, -60, z1)  # west hedge, alcoves between
        for z in (-9, -23, -51):
            m.det((-64.5, 0, z - 1.5), (-61.5, 3, z + 1.5), STONE); prism(m, -63, z, 0.8, 3, 7, 4, STONE)
        hedge(m, -50, -40, -42, -12); hedge(m, -60, -80, -42, -66)                            # east hedge, pocket's north end
        for z in range(4, -37, -6):
            for x in (-59.6, -50.4): prism(m, x, z, 0.4, 0, 10.1, 6, "dev/wood")
        for z in range(5, -36, -3): m.det((-60, 10.1, z - 0.2), (-50, 10.6, z + 0.2), "dev/wood")
        for x0 in (-60, -50.6): m.det((x0, 10.6, -36), (x0 + 0.6, 11.0, 6), "dev/wood")
        prism(m, -51, -53, 3.5, 0, 1.0, 12, STONE); prism(m, -51, -53, 1.0, 1.0, 10, 4, STONE)    # pocket: dry basin + obelisk
        m.det((-58, 0, -46), (-55, 2.5, -43), HEDGE); m.det((-47, 0, -64), (-44, 2.5, -61), HEDGE)
    else:
        hedge(m, -42, -128, 48, -126, 10); hedge(m, -42, -128, -40, -12, 10)
    hedge(m, 46, -128, 48, -40, 10)
    for a, b, c, d in ((-24, -82, -6, -80), (6, -82, 24, -82 + 2), (-26, -126, -24, -80), (24, -126, 26, -80),
                       (-40, -46, 26, -44), (38, -46, 46, -44), (-40, -82, -24, -80), (26, -82, 46, -80)):
        hedge(m, a, b, c, d)
    prism(m, 0, -103, 7, 0, 1.6, 12, LAWN); prism(m, 0, -103, 0.8, 1.6, 4.5, 6, STONE)       # rose bed + sundial
    for x in (-30, -18, -6, 6, 18): topiary(m, x, -70); topiary(m, x + 6, -56)
    m.det((-3, 0, -126), (3, 0.05, -82), PATH)
    # conservatory 28 x 28, 12 m glass walls, glass hip roof, raised plant beds; door to the east salon
    m.det((18, 0, -40), (46, FL, -12), STONE)
    for a, b in (((18, -40), (18.3, -12)), ((45.7, -40), (46, -12)), ((18, -40), (29, -39.7)), ((35, -40), (46, -39.7))):
        m.det((a[0], FL, a[1]), (b[0], FL + 12, b[1]), GLASS)
    roof(m, 18, -40, 46, -12, FL + 12, 6, 14, GLASS)
    for x, z in ((25, -32), (39, -32), (32, -21)): m.det((x - 3, FL, z - 3), (x + 3, FL + 1.8, z + 3), LAWN)
    m.stairs(29, 35, -36, -1, 0, FL, riser=0.3, tread=0.6)
    m.cut((30, FL, -12.1), (35, FL + 6.5, -9.9))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, fn in (("mansion_a", variant_a), ("mansion_b", variant_b)):
        mm = fn(); json.dump(mm.doc(), open(os.path.join(OUT, f"{name}.map.json"), "w"))
        print(name, "brushes", len(mm.world) + len(mm.detail))
