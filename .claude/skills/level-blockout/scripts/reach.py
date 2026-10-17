"""Reachability on a column grid, computed lazily (only columns the search touches). A column's solid = (worldspawn adds -
worldspawn subtractors) + func_detail + closed func_doors, by AABB (hull brushes count as their boxes: conservative).
A floor = top of a solid span with >= 1.8 m free above. Moves to a neighbouring column: up at most JUMP, down any height,
with head room over the whole movement."""
from collections import deque

CELL, JUMP, H, TILE = 0.5, 5.4, 1.8, 4.0


def union(iv):
    out = []
    for a, b in sorted((float(a), float(b)) for a, b in iv):
        if out and a <= out[-1][1] + 1e-6: out[-1][1] = max(out[-1][1], b)
        else: out.append([a, b])
    return out


def minus(U, S):
    out = []
    for a, b in U:
        cur = [[a, b]]
        for s0, s1 in S:
            nxt = []
            for c0, c1 in cur:
                if s1 <= c0 or s0 >= c1: nxt.append([c0, c1]); continue
                if s0 > c0: nxt.append([c0, s0])
                if s1 < c1: nxt.append([s1, c1])
            cur = nxt
        out += cur
    return out


class Model:
    def __init__(self, doc, open_doors=(), bounds=(-84, 84, -130, 192)):
        self.x0, self.x1, self.z0, self.z1 = bounds
        self.tiles, self.cache = {}, {}
        for e in doc["entities"]:
            c = e.get("classname")
            for b in e.get("brushes", []):
                if c == "worldspawn": self._add("s" if b.get("op") == "subtract" else "a", b)
                elif c == "func_detail" or (c == "func_door" and e["keyvalues"].get("targetname") not in open_doors): self._add("o", b)

    def _add(self, kind, b):
        vs = b["vertices"]; mn = [min(v[k] for v in vs) for k in range(3)]; mx = [max(v[k] for v in vs) for k in range(3)]
        for tx in range(int((mn[0] - self.x0) // TILE), int((mx[0] - self.x0) // TILE) + 1):
            for tz in range(int((mn[2] - self.z0) // TILE), int((mx[2] - self.z0) // TILE) + 1):
                self.tiles.setdefault((tx, tz), {"a": [], "s": [], "o": []})[kind].append((mn, mx))

    def col(self, c):
        r = self.cache.get(c)
        if r is not None: return r
        i, j = c; x = self.x0 + i * CELL; z = self.z0 + j * CELL; cx, cz = x + CELL / 2, z + CELL / 2
        t = self.tiles.get((int((cx - self.x0) // TILE), int((cz - self.z0) // TILE)))
        if not t or not (self.x0 <= cx <= self.x1 and self.z0 <= cz <= self.z1): r = ([], [])
        else:
            ov = lambda L: [(mn[1], mx[1]) for mn, mx in L if mn[0] < x + CELL and mx[0] > x and mn[2] < z + CELL and mx[2] > z]
            S = union([(mn[1], mx[1]) for mn, mx in t["s"] if mn[0] <= cx <= mx[0] and mn[2] <= cz <= mx[2]])
            sol = union(minus(union(ov(t["a"])), S) + ov(t["o"]))
            fl = [b for k, (a, b) in enumerate(sol) if b > -1 and (sol[k + 1][0] if k + 1 < len(sol) else 1e9) - b >= H]
            r = (sol, fl)
        self.cache[c] = r
        return r

    def cell(self, x, z): return int((x - self.x0) // CELL), int((z - self.z0) // CELL)

    def floor_at(self, x, z, y):
        fl = self.col(self.cell(x, z))[1]
        below = [h for h in fl if h <= y + 0.6]
        return max(below) if below else (fl[0] if fl else y)

    def free(self, c, lo, hi): return all(not (a < hi - 1e-6 and b > lo + 1e-6) for a, b in self.col(c)[0])

    def reach(self, x, z, y):
        c = self.cell(x, z); h = self.floor_at(x, z, y)
        seen = {(c, h)}; q = deque([(c, h)])
        while q:
            (i, j), h1 = q.popleft()
            for n in ((i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)):
                for h2 in self.col(n)[1]:
                    if h2 > h1 + JUMP: break
                    top = max(h1, h2) + H
                    if (n, h2) not in seen and self.free((i, j), h1, top) and self.free(n, h2, top):
                        seen.add((n, h2)); q.append((n, h2))
        return seen

    def at(self, seen, x, z, y):
        c = self.cell(x, z)
        return any(abs(h - y) <= 1.0 for (cc, h) in seen if cc == c)
