// tools/terrain -- see terrain.h.
#include "tools/terrain.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace native::tools::terrain {

namespace {

double q4(double v) { return std::round(v * 1e4) / 1e4; }   // heights are kept to 0.1 mm
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
size_t stride(const Spec& s) { return static_cast<size_t>(s.cols + 1); }

// The point k of the bottom ring (k = 0 .. 2 (w + d) - 1) as a grid point of the tile; the ring runs +x along z = 0, +z along x = w, -x along z = d, -z along x = 0.
std::array<int, 2> ringPoint(int w, int d, int k) {
    if (k < w) return {k, 0};
    if (k < w + d) return {w, k - w};
    if (k < 2 * w + d) return {w - (k - w - d), d};
    return {0, d - (k - 2 * w - d)};
}

template <class Height>
std::vector<core::Vec3> tileVerticesWith(const Spec& s, double ox, double oz, double base, int tx, int ty, Height height) {
    const TileCells c = tileCells(s, tx, ty);
    std::vector<core::Vec3> out;
    out.reserve(static_cast<size_t>((c.w + 1) * (c.d + 1) + 2 * (c.w + c.d)));
    for (int j = 0; j <= c.d; ++j)
        for (int i = 0; i <= c.w; ++i) out.emplace_back(ox + (c.x0 + i) * s.cell, height(c.x0 + i, c.z0 + j), oz + (c.z0 + j) * s.cell);
    for (int k = 0; k < 2 * (c.w + c.d); ++k) {
        const auto p = ringPoint(c.w, c.d, k);
        out.emplace_back(ox + (c.x0 + p[0]) * s.cell, base, oz + (c.z0 + p[1]) * s.cell);
    }
    return out;
}

std::vector<core::Vec3> tileVertices(const Heightfield& hf, int tx, int ty) {
    return tileVerticesWith(hf.spec, hf.ox, hf.oz, hf.base, tx, ty, [&hf](int gx, int gz) { return hf.at(gx, gz); });
}

size_t faceCount(int w, int d) { return static_cast<size_t>(2 * w * d + 2 * (w + d) + 1); }

// Faces of a tile (w x d cells at the global cell offset x0, z0): the triangulation is fixed by the cell's chessboard parity. Also the smooth pairs between the top triangles.
void tileFaces(int x0, int z0, int w, int d, const std::string& top, std::vector<core::BrushFace>& faces, std::vector<std::pair<uint32_t, uint32_t>>& smooth) {
    const auto g = [w](int i, int j) { return static_cast<uint32_t>(j * (w + 1) + i); };
    const uint32_t T0 = static_cast<uint32_t>((w + 1) * (d + 1));
    const int P = 2 * (w + d);
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeFace;
    const auto addTop = [&](uint32_t id, uint32_t a, uint32_t b, uint32_t c) {
        core::BrushFace f;
        f.id = id;
        f.loop = {a, b, c};
        f.texture = top;
        faces.push_back(std::move(f));
        const uint32_t pairs[3][2] = {{a, b}, {b, c}, {c, a}};
        for (const auto& e : pairs) {
            const std::pair<uint32_t, uint32_t> k{std::min(e[0], e[1]), std::max(e[0], e[1])};
            const auto other = edgeFace.find(k);
            if (other == edgeFace.end()) edgeFace.emplace(k, id);
            else smooth.emplace_back(other->second, id);
        }
    };
    for (int j = 0; j < d; ++j)
        for (int i = 0; i < w; ++i) {
            const uint32_t c = static_cast<uint32_t>(j * w + i), p00 = g(i, j), p10 = g(i + 1, j), p01 = g(i, j + 1), p11 = g(i + 1, j + 1);
            if ((x0 + i + z0 + j) % 2 == 0) {
                addTop(2 * c, p00, p01, p11);
                addTop(2 * c + 1, p00, p11, p10);
            } else {
                addTop(2 * c, p00, p01, p10);
                addTop(2 * c + 1, p01, p11, p10);
            }
        }
    const uint32_t W0 = static_cast<uint32_t>(2 * w * d);
    for (int k = 0; k < P; ++k) {
        const auto a = ringPoint(w, d, k), b = ringPoint(w, d, (k + 1) % P);
        core::BrushFace f;
        f.id = W0 + static_cast<uint32_t>(k);
        f.loop = {g(a[0], a[1]), g(b[0], b[1]), T0 + static_cast<uint32_t>((k + 1) % P), T0 + static_cast<uint32_t>(k)};
        f.texture = kSideTexture;
        faces.push_back(std::move(f));
    }
    core::BrushFace bottom;
    bottom.id = W0 + static_cast<uint32_t>(P);
    for (int k = 0; k < P; ++k) bottom.loop.push_back(T0 + static_cast<uint32_t>(k));
    bottom.texture = kSideTexture;
    faces.push_back(std::move(bottom));
    std::sort(smooth.begin(), smooth.end());
}

core::Brush buildTile(const Heightfield& hf, int tx, int ty, const std::string& id, const std::string& top) {
    const TileCells c = tileCells(hf.spec, tx, ty);
    core::Brush b;
    b.id = id;
    b.vertices = tileVertices(hf, tx, ty);
    tileFaces(c.x0, c.z0, c.w, c.d, top, b.faces, b.smoothEdges);
    return b;
}

constexpr double kEps = 1e-6;

// Does the brush hold exactly the tile these vertices describe (positions, vertex and face count)?
bool matches(const core::Brush& b, const std::vector<core::Vec3>& want, const Spec& s, int tx, int ty) {
    const TileCells c = tileCells(s, tx, ty);
    if (b.vertices.size() != want.size() || b.faces.size() != faceCount(c.w, c.d)) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        const core::Vec3 &p = b.vertices[i], &q = want[i];
        if (std::abs(p.x - q.x) > kEps || std::abs(p.y - q.y) > kEps || std::abs(p.z - q.z) > kEps) return false;
    }
    return true;
}

std::map<int, std::shared_ptr<const doc::BrushNode>> tilesOf(const doc::EntityNode& e, const Spec& spec, bool& bad) {
    std::map<int, std::shared_ptr<const doc::BrushNode>> tiles;
    const int nx = tilesAcross(spec), ny = tilesDown(spec);
    bad = false;
    for (const auto& b : e.brushes) {
        const auto t = tileOf(b->raw);
        if (!t) continue;
        if ((*t)[0] >= nx || (*t)[1] >= ny || tiles.count((*t)[1] * nx + (*t)[0])) {
            bad = true;
            return tiles;
        }
        tiles[(*t)[1] * nx + (*t)[0]] = b;
    }
    return tiles;
}

std::shared_ptr<const Heightfield> readTerrain(const doc::EntityNode& e) {
    const auto spec = specOf(e.raw);
    if (!spec) return nullptr;
    const int nx = tilesAcross(*spec), ny = tilesDown(*spec);
    bool bad = false;
    const auto tiles = tilesOf(e, *spec, bad);
    if (bad || tiles.size() != static_cast<size_t>(nx * ny)) return nullptr;
    const auto& first = tiles.at(0)->brush;
    const TileCells c0 = tileCells(*spec, 0, 0);
    const size_t T0 = static_cast<size_t>((c0.w + 1) * (c0.d + 1));
    if (first.vertices.size() <= T0) return nullptr;
    auto hf = std::make_shared<Heightfield>();
    hf->spec = *spec;
    hf->ox = first.vertices[0].x;
    hf->oz = first.vertices[0].z;
    hf->base = first.vertices[T0].y;
    hf->h.assign(static_cast<size_t>((spec->cols + 1) * (spec->rows + 1)), 0.0);
    std::vector<char> seen(hf->h.size(), 0);
    for (int ty = 0; ty < ny; ++ty)
        for (int tx = 0; tx < nx; ++tx) {
            const TileCells c = tileCells(*spec, tx, ty);
            const core::Brush& b = tiles.at(ty * nx + tx)->brush;
            if (b.vertices.size() < static_cast<size_t>((c.w + 1) * (c.d + 1))) return nullptr;
            for (int j = 0; j <= c.d; ++j)
                for (int i = 0; i <= c.w; ++i) {
                    const double y = b.vertices[static_cast<size_t>(j * (c.w + 1) + i)].y;
                    const size_t at = static_cast<size_t>(c.z0 + j) * stride(*spec) + static_cast<size_t>(c.x0 + i);
                    if (seen[at] && std::abs(hf->h[at] - y) > kEps) return nullptr;   // a seam that does not close
                    hf->h[at] = y;
                    seen[at] = 1;
                }
            if (!matches(b, tileVertices(*hf, tx, ty), *spec, tx, ty)) return nullptr;
        }
    return hf;
}

// A terrain entity that is still what its record says, as far as the tiles that changed are concerned.
bool validEntity(const doc::EntityNode& e, const doc::EntityNode* before) {
    const auto spec = specOf(e.raw);
    if (!spec) return false;
    const int nx = tilesAcross(*spec), ny = tilesDown(*spec);
    for (const auto& b : e.brushes)
        if (b->raw.is_object() && b->raw.contains("terrainTile") && !tileOf(b->raw)) return false;
    bool bad = false;
    const auto tiles = tilesOf(e, *spec, bad);
    if (bad || tiles.size() != static_cast<size_t>(nx * ny)) return false;
    const auto& first = tiles.at(0)->brush;
    const TileCells c0 = tileCells(*spec, 0, 0);
    const size_t T0 = static_cast<size_t>((c0.w + 1) * (c0.d + 1));
    if (first.vertices.size() <= T0) return false;
    const double ox = first.vertices[0].x, oz = first.vertices[0].z, base = first.vertices[T0].y;
    std::map<std::string, const doc::BrushNode*> prevBrush;
    if (before)
        for (const auto& b : before->brushes) prevBrush[b->brush.id] = b.get();
    const auto topAt = [&](const core::Brush& b, int tx, int ty, int gx, int gz) {
        const TileCells c = tileCells(*spec, tx, ty);
        return b.vertices[static_cast<size_t>((gz - c.z0) * (c.w + 1) + (gx - c.x0))].y;
    };
    for (const auto& [index, node] : tiles) {
        const int tx = index % nx, ty = index / nx;
        const auto was = prevBrush.find(node->brush.id);
        if (was != prevBrush.end() && was->second == node.get()) continue;
        // this tile changed: its own top heights define what it must look like
        const core::Brush& b = node->brush;
        const TileCells c = tileCells(*spec, tx, ty);
        if (b.vertices.size() < static_cast<size_t>((c.w + 1) * (c.d + 1))) return false;
        const auto own = tileVerticesWith(*spec, ox, oz, base, tx, ty, [&](int gx, int gz) { return topAt(b, tx, ty, gx, gz); });
        if (!matches(b, own, *spec, tx, ty)) return false;
        // the border it shares with a neighbour must still close
        for (const auto& [nIndex, nNode] : tiles) {
            const int ntx = nIndex % nx, nty = nIndex / nx;
            if (std::abs(ntx - tx) + std::abs(nty - ty) != 1) continue;
            const TileCells a = c, d = tileCells(*spec, ntx, nty);
            const bool alongZ = ntx != tx;   // a vertical seam: the points run along z
            const int line = alongZ ? std::max(tx, ntx) * spec->tile : std::max(ty, nty) * spec->tile;
            const int from = alongZ ? std::max(a.z0, d.z0) : std::max(a.x0, d.x0);
            const int len = alongZ ? std::min(a.d, d.d) : std::min(a.w, d.w);
            for (int k = 0; k <= len; ++k) {
                const int gx = alongZ ? line : from + k, gz = alongZ ? from + k : line;
                if (std::abs(topAt(b, tx, ty, gx, gz) - topAt(nNode->brush, ntx, nty, gx, gz)) > kEps) return false;
            }
        }
    }
    return true;
}

} // namespace

// ---- grid ------------------------------------------------------------------------------------------------------------------------------

int tilesAcross(const Spec& s) { return (s.cols + s.tile - 1) / s.tile; }
int tilesDown(const Spec& s) { return (s.rows + s.tile - 1) / s.tile; }

TileCells tileCells(const Spec& s, int tx, int ty) {
    TileCells c;
    c.x0 = tx * s.tile;
    c.z0 = ty * s.tile;
    c.w = std::min(s.tile, s.cols - c.x0);
    c.d = std::min(s.tile, s.rows - c.z0);
    return c;
}

std::optional<Spec> specOf(const Json& e) {
    if (!e.is_object()) return std::nullopt;
    const auto it = e.find("terrain");
    if (it == e.end() || !it->is_object()) return std::nullopt;
    const Json &cols = (*it).value("cols", Json()), rows = (*it).value("rows", Json()), cell = (*it).value("cell", Json()), tile = (*it).value("tile", Json());
    if (!cols.is_number_integer() || !rows.is_number_integer() || !tile.is_number_integer() || !cell.is_number()) return std::nullopt;
    Spec s;
    s.cols = cols.get<int>();
    s.rows = rows.get<int>();
    s.tile = tile.get<int>();
    s.cell = cell.get<double>();
    if (s.cols < 1 || s.rows < 1 || s.cols > kMaxCellsPerSide || s.rows > kMaxCellsPerSide || s.tile < 1 || !(s.cell > 0) || !std::isfinite(s.cell)) return std::nullopt;
    return s;
}

Json specJson(const Spec& s) { return Json{{"cols", s.cols}, {"rows", s.rows}, {"cell", s.cell}, {"tile", s.tile}}; }

std::optional<std::array<int, 2>> tileOf(const Json& b) {
    if (!b.is_object()) return std::nullopt;
    const auto it = b.find("terrainTile");
    if (it == b.end() || !it->is_array() || it->size() != 2) return std::nullopt;
    if (!(*it)[0].is_number_integer() || !(*it)[1].is_number_integer()) return std::nullopt;
    const int x = (*it)[0].get<int>(), y = (*it)[1].get<int>();
    if (x < 0 || y < 0) return std::nullopt;
    return std::array<int, 2>{x, y};
}

std::string describeSpec(const Spec& s) {
    std::ostringstream o;
    const auto num = [](double v) {
        std::ostringstream n;
        n << std::defaultfloat << std::setprecision(6) << v;
        return n.str();
    };
    o << num(s.cols * s.cell) << " x " << num(s.rows * s.cell) << " m, " << s.cols << " x " << s.rows << " cells of " << num(s.cell) << " m, " << tilesAcross(s) * tilesDown(s)
      << " tiles (brushes), " << static_cast<long long>(s.cols) * s.rows * 2 << " top triangles";
    return o.str();
}

std::shared_ptr<const Heightfield> terrainOf(const std::shared_ptr<const doc::EntityNode>& e) {
    static std::map<const doc::EntityNode*, std::pair<std::weak_ptr<const doc::EntityNode>, std::shared_ptr<const Heightfield>>> cache;
    if (!e) return nullptr;
    const auto hit = cache.find(e.get());
    if (hit != cache.end() && hit->second.first.lock() == e) return hit->second.second;
    if (cache.size() > 16) {
        for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
        if (cache.size() > 16) cache.clear();
    }
    auto r = readTerrain(*e);
    cache[e.get()] = {e, r};
    return r;
}

std::string cellTexture(const doc::EntityNode& e, const Spec& spec, int gi, int gj) {
    const int tx = gi / spec.tile, ty = gj / spec.tile;
    for (const auto& b : e.brushes) {
        const auto t = tileOf(b->raw);
        if (!t || (*t)[0] != tx || (*t)[1] != ty) continue;
        const TileCells c = tileCells(spec, tx, ty);
        const size_t face = static_cast<size_t>(2 * ((gj - c.z0) * c.w + (gi - c.x0)));
        return face < b->brush.faces.size() ? b->brush.faces[face].texture : std::string();
    }
    return {};
}

// ---- creating --------------------------------------------------------------------------------------------------------------------------

std::optional<Spec> gridFor(const CreateParams& p, std::string& error) {
    error.clear();
    if (!(p.cell >= 0.05)) {
        error = "The cell size must be at least 0.05 m";
        return std::nullopt;
    }
    const double cols = std::round(p.sizeX / p.cell), rows = std::round(p.sizeZ / p.cell);
    if (!(cols >= 1) || !(rows >= 1)) {
        error = "The terrain must be at least one cell big";
        return std::nullopt;
    }
    if (cols > kMaxCellsPerSide || rows > kMaxCellsPerSide || cols * rows > kMaxCells) {
        error = "Too many cells (" + std::to_string(static_cast<long long>(cols)) + " x " + std::to_string(static_cast<long long>(rows)) + "): at most " + std::to_string(kMaxCells) +
                " cells and " + std::to_string(kMaxCellsPerSide) + " per side; use a bigger cell";
        return std::nullopt;
    }
    if (!(p.tile >= 2 && p.tile <= 64)) {
        error = "A tile is 2 to 64 cells wide";
        return std::nullopt;
    }
    if (!(p.thickness >= 1)) {
        error = "The slab needs a thickness of at least 1 m";
        return std::nullopt;
    }
    Spec s;
    s.cols = static_cast<int>(cols);
    s.rows = static_cast<int>(rows);
    s.cell = p.cell;
    s.tile = p.tile;
    return s;
}

bool addTerrain(doc::VersionEditor& ed, const CreateParams& p, const std::string& entityId, const std::function<std::string()>& newBrushId, std::string& error) {
    const auto spec = gridFor(p, error);
    if (!spec) return false;
    Heightfield hf;
    hf.spec = *spec;
    hf.ox = p.origin[0];
    hf.oz = p.origin[2];
    hf.base = p.origin[1] - p.thickness;
    hf.h.assign(static_cast<size_t>((spec->cols + 1) * (spec->rows + 1)), p.origin[1]);
    core::Entity e;
    e.id = entityId;
    e.classname = "func_detail";
    e.origin = core::Vec3(hf.ox + spec->cols * spec->cell / 2, (p.origin[1] + hf.base) / 2, hf.oz + spec->rows * spec->cell / 2);
    if (!ed.addEntity(e)) {
        error = ed.error();
        return false;
    }
    for (int ty = 0; ty < tilesDown(*spec); ++ty)
        for (int tx = 0; tx < tilesAcross(*spec); ++tx) {
            const core::Brush b = buildTile(hf, tx, ty, newBrushId(), p.texture);
            if (!ed.addBrush(entityId, b) || !ed.setBrushKey(entityId, b.id, "terrainTile", Json::array({tx, ty}))) {
                error = ed.error();
                return false;
            }
        }
    if (!ed.setEntityKey(entityId, "terrain", specJson(*spec))) {
        error = ed.error();
        return false;
    }
    return true;
}

// ---- keeping the record honest ---------------------------------------------------------------------------------------------------------

std::shared_ptr<const doc::DocumentVersion> reconcileTerrain(const std::shared_ptr<const doc::DocumentVersion>& prev, const std::shared_ptr<const doc::DocumentVersion>& next) {
    if (!next) return next;
    std::vector<std::shared_ptr<const doc::EntityNode>> strip;
    for (const auto& e : next->entities) {
        const bool record = e->raw.is_object() && e->raw.contains("terrain");
        bool marked = false;
        for (const auto& b : e->brushes)
            if (b->raw.is_object() && b->raw.contains("terrainTile")) {
                marked = true;
                break;
            }
        if (!record && !marked) continue;
        const int pi = prev ? prev->findEntity(e->typed.id) : -1;
        if (pi >= 0 && prev->entities[static_cast<size_t>(pi)] == e) continue;
        if (!validEntity(*e, pi >= 0 ? prev->entities[static_cast<size_t>(pi)].get() : nullptr)) strip.push_back(e);
    }
    if (strip.empty()) return next;
    doc::VersionEditor ed(next);
    for (const auto& e : strip) {
        ed.setEntityKey(e->typed.id, "terrain", Json());
        for (const auto& b : e->brushes)
            if (b->raw.is_object() && b->raw.contains("terrainTile")) ed.setBrushKey(e->typed.id, b->brush.id, "terrainTile", Json());
    }
    const auto fixed = ed.finish();
    return fixed ? fixed : next;
}

// ---- sampling --------------------------------------------------------------------------------------------------------------------------

double heightAt(const Heightfield& hf, double x, double z) {
    const Spec& s = hf.spec;
    const double gx = clampd((x - hf.ox) / s.cell, 0, s.cols), gz = clampd((z - hf.oz) / s.cell, 0, s.rows);
    const int i = std::min(static_cast<int>(std::floor(gx)), s.cols - 1), j = std::min(static_cast<int>(std::floor(gz)), s.rows - 1);
    const double fx = gx - i, fz = gz - j;
    const double h00 = hf.at(i, j), h10 = hf.at(i + 1, j), h01 = hf.at(i, j + 1), h11 = hf.at(i + 1, j + 1);
    if ((i + j) % 2 == 0) return fz >= fx ? h00 + fx * (h11 - h01) + fz * (h01 - h00) : h00 + fx * (h10 - h00) + fz * (h11 - h10);
    return fx + fz <= 1 ? h00 + fx * (h10 - h00) + fz * (h01 - h00) : h11 + (1 - fx) * (h01 - h11) + (1 - fz) * (h10 - h11);
}

namespace {

V3 sub3(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross3(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

std::optional<double> rayTri(const V3& o, const V3& dir, const V3& a, const V3& b, const V3& c) {
    const V3 e1 = sub3(b, a), e2 = sub3(c, a), p = cross3(dir, e2);
    const double det = dot3(e1, p);
    if (std::abs(det) < 1e-14) return std::nullopt;
    const double inv = 1 / det;
    const V3 tv = sub3(o, a);
    const double u = dot3(tv, p) * inv;
    if (u < -1e-9 || u > 1 + 1e-9) return std::nullopt;
    const V3 qv = cross3(tv, e1);
    const double v = dot3(dir, qv) * inv;
    if (v < -1e-9 || u + v > 1 + 1e-9) return std::nullopt;
    const double t = dot3(e2, qv) * inv;
    if (t > 0) return t;
    return std::nullopt;
}

} // namespace

std::optional<RayHit> rayTerrain(const Heightfield& hf, const V3& origin, const V3& dir) {
    const Spec& s = hf.spec;
    const double W = s.cols * s.cell, D = s.rows * s.cell;
    double lo = 0, hi = std::numeric_limits<double>::infinity();
    const auto slab = [&](double o, double dv, double a, double b) {
        if (std::abs(dv) < 1e-12) return o >= a && o <= b;
        double t0 = (a - o) / dv, t1 = (b - o) / dv;
        if (t0 > t1) std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
        return lo <= hi;
    };
    if (!slab(origin[0], dir[0], hf.ox, hf.ox + W) || !slab(origin[2], dir[2], hf.oz, hf.oz + D) || !slab(origin[1], dir[1], hf.base, 1e9)) return std::nullopt;
    // walk the cells of the column the ray crosses between lo and hi (Amanatides-Woo)
    const double px = origin[0] + dir[0] * lo, pz = origin[2] + dir[2] * lo;
    int i = static_cast<int>(clampd(std::floor((px - hf.ox) / s.cell), 0, s.cols - 1)), j = static_cast<int>(clampd(std::floor((pz - hf.oz) / s.cell), 0, s.rows - 1));
    const int sx = dir[0] > 0 ? 1 : -1, sz = dir[2] > 0 ? 1 : -1;
    const double inf = std::numeric_limits<double>::infinity();
    const double nextX = (hf.ox + (i + (sx > 0 ? 1 : 0)) * s.cell - origin[0]) / dir[0], nextZ = (hf.oz + (j + (sz > 0 ? 1 : 0)) * s.cell - origin[2]) / dir[2];
    double tMaxX = std::abs(dir[0]) < 1e-12 ? inf : nextX, tMaxZ = std::abs(dir[2]) < 1e-12 ? inf : nextZ;
    const double tDX = std::abs(dir[0]) < 1e-12 ? inf : std::abs(s.cell / dir[0]), tDZ = std::abs(dir[2]) < 1e-12 ? inf : std::abs(s.cell / dir[2]);
    for (int guard = 0; guard < s.cols + s.rows + 4; ++guard) {
        std::optional<double> best;
        const auto P = [&](int a, int b) { return V3{hf.ox + (i + a) * s.cell, hf.at(i + a, j + b), hf.oz + (j + b) * s.cell}; };
        const V3 p00 = P(0, 0), p10 = P(1, 0), p01 = P(0, 1), p11 = P(1, 1);
        const std::array<std::array<V3, 3>, 2> tris = ((i + j) % 2 == 0) ? std::array<std::array<V3, 3>, 2>{{{p00, p01, p11}, {p00, p11, p10}}}
                                                                          : std::array<std::array<V3, 3>, 2>{{{p00, p01, p10}, {p01, p11, p10}}};
        for (const auto& t : tris) {
            const auto h = rayTri(origin, dir, t[0], t[1], t[2]);
            if (h && (!best || *h < *best)) best = h;
        }
        if (best) return RayHit{*best, {origin[0] + dir[0] * *best, origin[1] + dir[1] * *best, origin[2] + dir[2] * *best}};
        if (tMaxX < tMaxZ) {
            if (tMaxX > hi) break;
            i += sx;
            tMaxX += tDX;
        } else {
            if (tMaxZ > hi) break;
            j += sz;
            tMaxZ += tDZ;
        }
        if (i < 0 || i >= s.cols || j < 0 || j >= s.rows) break;
    }
    return std::nullopt;
}

std::vector<double> circleOnTerrain(const Heightfield& hf, double cx, double cz, double r, int segments, double lift) {
    std::vector<double> out;
    const auto pt = [&](int k) {
        const double a = static_cast<double>(k) / segments * 6.283185307179586, x = cx + std::cos(a) * r, z = cz + std::sin(a) * r;
        return V3{x, heightAt(hf, x, z) + lift, z};
    };
    V3 prev = pt(0);
    for (int k = 1; k <= segments; ++k) {
        const V3 p = pt(k % segments);
        out.insert(out.end(), prev.begin(), prev.end());
        out.insert(out.end(), p.begin(), p.end());
        prev = p;
    }
    return out;
}

// ---- brushes ---------------------------------------------------------------------------------------------------------------------------

double brushWeight(double d, double radius, double falloff) {
    if (d >= radius) return 0;
    const double inner = radius * (1 - clampd(falloff, 0, 1));
    if (d <= inner) return 1;
    const double t = (d - inner) / (radius - inner);
    return 1 - t * t * (3 - 2 * t);
}

namespace {

std::optional<PointRect> rectAround(const Heightfield& hf, double cx, double cz, double r) {
    const Spec& s = hf.spec;
    PointRect p;
    p.gx0 = std::max(0, static_cast<int>(std::ceil((cx - r - hf.ox) / s.cell)));
    p.gx1 = std::min(s.cols, static_cast<int>(std::floor((cx + r - hf.ox) / s.cell)));
    p.gz0 = std::max(0, static_cast<int>(std::ceil((cz - r - hf.oz) / s.cell)));
    p.gz1 = std::min(s.rows, static_cast<int>(std::floor((cz + r - hf.oz) / s.cell)));
    if (p.gx0 > p.gx1 || p.gz0 > p.gz1) return std::nullopt;
    return p;
}

} // namespace

std::optional<PointRect> stamp(Heightfield& hf, Tool tool, double cx, double cz, const BrushParams& p, const StampOptions& opts) {
    const Spec& s = hf.spec;
    const double r = p.radius;
    const auto rect = rectAround(hf, cx, cz, r);
    if (!rect || !(r > 0) || tool == Tool::Paint) return std::nullopt;
    const double lo = hf.base + kMinThickness, hi = hf.base + 100000;
    const double sign = (tool == Tool::Lower ? -1 : 1) * (opts.invert ? -1 : 1);
    std::vector<std::array<int, 2>> out;
    std::vector<double> weights;
    for (int gz = rect->gz0; gz <= rect->gz1; ++gz)
        for (int gx = rect->gx0; gx <= rect->gx1; ++gx) {
            const double w = brushWeight(std::hypot(hf.ox + gx * s.cell - cx, hf.oz + gz * s.cell - cz), r, p.falloff);
            weights.push_back(w);
            if (w > 0) out.push_back({gx, gz});
        }
    if (out.empty()) return std::nullopt;
    const auto wAt = [&](int gx, int gz) { return weights[static_cast<size_t>((gz - rect->gz0) * (rect->gx1 - rect->gx0 + 1) + (gx - rect->gx0))]; };
    const auto idx = [&](int gx, int gz) { return static_cast<size_t>(gz) * stride(s) + static_cast<size_t>(gx); };
    if (tool == Tool::Smooth) {
        // read the old heights, write the new ones (so the result does not depend on the order of the points)
        std::vector<std::pair<size_t, double>> next;
        for (const auto& pt : out) {
            double sum = 0;
            int n = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = pt[0] + dx, z = pt[1] + dz;
                    if (x < 0 || z < 0 || x > s.cols || z > s.rows) continue;
                    sum += hf.h[idx(x, z)];
                    ++n;
                }
            const double f = clampd(p.strength * wAt(pt[0], pt[1]), 0, 1), cur = hf.h[idx(pt[0], pt[1])];
            next.emplace_back(idx(pt[0], pt[1]), cur + (sum / n - cur) * f);
        }
        for (const auto& [at, y] : next) hf.h[at] = clampd(q4(y), lo, hi);
    } else {
        for (const auto& pt : out) {
            const size_t at = idx(pt[0], pt[1]);
            const double w = wAt(pt[0], pt[1]);
            double y = hf.h[at];
            if (tool == Tool::Flatten) y += (opts.target.value_or(y) - y) * clampd(p.strength * w, 0, 1);
            else y += sign * p.strength * kRaisePerStrength * w;
            hf.h[at] = clampd(q4(y), lo, hi);
        }
    }
    PointRect r2{out[0][0], out[0][0], out[0][1], out[0][1]};
    for (const auto& pt : out) {
        r2.gx0 = std::min(r2.gx0, pt[0]);
        r2.gx1 = std::max(r2.gx1, pt[0]);
        r2.gz0 = std::min(r2.gz0, pt[1]);
        r2.gz1 = std::max(r2.gz1, pt[1]);
    }
    return r2;
}

std::vector<int> tilesOfRect(const Spec& s, const PointRect& r) {
    const int nx = tilesAcross(s), ny = tilesDown(s), T = s.tile;
    const auto range = [T](int a, int b, int n) { return std::array<int, 2>{std::max(0, (a + T - 1) / T - 1), std::min(n - 1, b / T)}; };
    const auto rx = range(r.gx0, r.gx1, nx), rz = range(r.gz0, r.gz1, ny);
    std::vector<int> out;
    for (int ty = rz[0]; ty <= rz[1]; ++ty)
        for (int tx = rx[0]; tx <= rx[1]; ++tx) out.push_back(ty * nx + tx);
    return out;
}

std::vector<int> cellsInRadius(const Heightfield& hf, double cx, double cz, double radius) {
    const Spec& s = hf.spec;
    const int i0 = std::max(0, static_cast<int>(std::floor((cx - radius - hf.ox) / s.cell))), i1 = std::min(s.cols - 1, static_cast<int>(std::floor((cx + radius - hf.ox) / s.cell)));
    const int j0 = std::max(0, static_cast<int>(std::floor((cz - radius - hf.oz) / s.cell))), j1 = std::min(s.rows - 1, static_cast<int>(std::floor((cz + radius - hf.oz) / s.cell)));
    std::vector<int> out;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (std::hypot(hf.ox + (i + 0.5) * s.cell - cx, hf.oz + (j + 0.5) * s.cell - cz) <= radius) out.push_back(j * s.cols + i);
    return out;
}

// ---- a stroke --------------------------------------------------------------------------------------------------------------------------

Stroke::Stroke(const std::string& entityId, std::shared_ptr<const Heightfield> hf, Tool tool, const BrushParams& params, const StrokeOptions& opts)
    : entityId_(entityId), hf_(*hf), tool_(tool), params_(params), opts_(opts), invert_(opts.invert) {}

double Stroke::spacing() const { return std::max(params_.radius * 0.2, hf_.spec.cell * 0.25); }

void Stroke::stampAt(double x, double z) {
    const Spec& s = hf_.spec;
    if (tool_ == Tool::Paint) {
        if (opts_.texture.empty()) return;
        for (int c : cellsInRadius(hf_, x, z, params_.radius)) {
            const int gi = c % s.cols, gj = c / s.cols;
            const int t = (gj / s.tile) * tilesAcross(s) + gi / s.tile;
            const auto it = painted_.find(c);
            if (it == painted_.end() || it->second != opts_.texture) {
                painted_[c] = opts_.texture;
                paintDirty_.insert(t);
            }
        }
        return;
    }
    if (tool_ == Tool::Flatten && !target_) target_ = opts_.flattenHeight ? *opts_.flattenHeight : heightAt(hf_, x, z);
    StampOptions so;
    so.invert = invert_;
    so.target = target_;
    if (const auto r = stamp(hf_, tool_, x, z, params_, so))
        for (int t : tilesOfRect(s, *r)) heightDirty_.insert(t);
}

void Stroke::moveTo(double x, double z) {
    if (!last_) {
        stampAt(x, z);
        last_ = std::array<double, 2>{x, z};
        return;
    }
    const double lx = (*last_)[0], lz = (*last_)[1], len = std::hypot(x - lx, z - lz), sp = spacing();
    double at = sp - carry_;
    while (at <= len) {
        const double f = at / len;
        stampAt(lx + (x - lx) * f, lz + (z - lz) * f);
        at += sp;
    }
    carry_ = len - (at - sp);
    last_ = std::array<double, 2>{x, z};
}

void Stroke::repeat() {
    if (last_) stampAt((*last_)[0], (*last_)[1]);
}

std::shared_ptr<const doc::DocumentVersion> Stroke::document(const std::shared_ptr<const doc::DocumentVersion>& start) {
    if (!start) return start;
    const bool resume = lastStart_ == start && lastResult_;
    if (heightDirty_.empty() && paintDirty_.empty()) return resume ? lastResult_ : start;
    const std::shared_ptr<const doc::DocumentVersion> base = resume ? lastResult_ : start;
    const int ei = base->findEntity(entityId_);
    if (ei < 0) return resume ? lastResult_ : start;
    const auto& node = base->entities[static_cast<size_t>(ei)];
    const Spec& s = hf_.spec;
    const int nx = tilesAcross(s);
    std::map<int, const doc::BrushNode*> tiles;
    for (const auto& b : node->brushes)
        if (const auto t = tileOf(b->raw)) tiles[(*t)[1] * nx + (*t)[0]] = b.get();
    std::set<int> dirty = heightDirty_;
    dirty.insert(paintDirty_.begin(), paintDirty_.end());
    doc::VersionEditor ed(base);
    for (int t : dirty) {
        const auto it = tiles.find(t);
        if (it == tiles.end()) continue;
        const int tx = t % nx, ty = t / nx;
        core::Brush b = it->second->brush;
        if (heightDirty_.count(t)) b.vertices = tileVertices(hf_, tx, ty);
        if (paintDirty_.count(t)) {
            const TileCells c = tileCells(s, tx, ty);
            const uint32_t top = static_cast<uint32_t>(2 * c.w * c.d);
            for (core::BrushFace& f : b.faces) {
                if (f.id >= top) continue;
                const int cell = static_cast<int>(f.id >> 1), gi = c.x0 + cell % c.w, gj = c.z0 + cell / c.w;
                const auto p = painted_.find(gj * s.cols + gi);
                if (p != painted_.end() && p->second != f.texture) f.texture = p->second;
            }
        }
        ed.replaceBrush(entityId_, b);
    }
    const auto result = ed.finish();
    if (!result) return resume ? lastResult_ : start;
    heightDirty_.clear();
    paintDirty_.clear();
    done_.insert(dirty.begin(), dirty.end());
    lastStart_ = start;
    lastResult_ = result;
    return result;
}

} // namespace native::tools::terrain
