// T11.8a: terrain (tools/terrain), converted from shell/tests/terrain.test.ts: creating, the height brushes, strokes, keeping the record honest and sampling the surface.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/terrain.h"
#include "tools/transform.h"

using namespace native;
namespace tr = native::tools::terrain;
using Version = std::shared_ptr<const doc::DocumentVersion>;
using tr::Json;

namespace {

int counter = 0;
std::string bid() { return "b" + std::to_string(counter++); }

tr::CreateParams params() {
    tr::CreateParams p;
    p.sizeX = 10;
    p.sizeZ = 6;
    p.cell = 1;
    p.tile = 4;
    p.origin = {-5, 2, -3};
    p.thickness = 4;
    p.texture = "dev/grass";
    return p;
}

Version make(const tr::CreateParams& p = params()) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    std::string error;
    INFO(error);
    REQUIRE(tr::addTerrain(ed, p, "terrain1", bid, error));
    const Version v = ed.finish();
    REQUIRE(v);
    return v;
}

std::shared_ptr<const doc::EntityNode> terrainNode(const Version& v, const std::string& id = "terrain1") {
    const int i = v->findEntity(id);
    REQUIRE(i >= 0);
    return v->entities[static_cast<size_t>(i)];
}

std::shared_ptr<const doc::BrushNode> tile(const Version& v, int tx, int ty, const std::string& entity = "terrain1") {
    for (const auto& b : terrainNode(v, entity)->brushes) {
        const auto t = tr::tileOf(b->raw);
        if (t && (*t)[0] == tx && (*t)[1] == ty) return b;
    }
    FAIL("no tile " << tx << "," << ty);
    return nullptr;
}

std::shared_ptr<const tr::Heightfield> hfOf(const Version& v, const std::string& entity = "terrain1") {
    const auto h = tr::terrainOf(terrainNode(v, entity));
    REQUIRE(h);
    return h;
}

// Every directed edge of the brush appears once and its reverse once: a closed 2-manifold.
bool closedManifold(const core::Brush& b) {
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const auto& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) ++edges[{f.loop[i], f.loop[(i + 1) % f.loop.size()]}];
    for (const auto& [k, c] : edges)
        if (c != 1 || !edges.count({k.second, k.first})) return false;
    return true;
}

double signedVolume(const core::Brush& b) {
    double v = 0;
    for (const auto& f : b.faces)
        for (size_t i = 1; i + 1 < f.loop.size(); ++i) {
            const core::Vec3 &a = b.vertices[f.loop[0]], &p = b.vertices[f.loop[i]], &q = b.vertices[f.loop[i + 1]];
            v += (a.x * (p.y * q.z - p.z * q.y) - a.y * (p.x * q.z - p.z * q.x) + a.z * (p.x * q.y - p.y * q.x)) / 6;
        }
    return v;
}

double maxOf(const std::vector<double>& h) { return *std::max_element(h.begin(), h.end()); }

Version commitStroke(const Version& v, tr::Stroke& s) { return s.document(v); }

int changedTiles(const Version& a, const Version& b) {
    int n = 0;
    const auto ea = terrainNode(a), eb = terrainNode(b);
    for (size_t i = 0; i < ea->brushes.size(); ++i)
        if (ea->brushes[i] != eb->brushes[i]) ++n;
    return n;
}

// a version with one tile of the terrain replaced by `brush`
Version withBrush(const Version& v, const core::Brush& brush) {
    doc::VersionEditor ed(v);
    REQUIRE(ed.replaceBrush("terrain1", brush));
    const Version r = ed.finish();
    REQUIRE(r);
    return r;
}

} // namespace

TEST_CASE("terrain: creating gives one func_detail entity of closed, planar slab tiles") {
    const Version v = make();
    const auto e = terrainNode(v);
    CHECK(e->typed.classname == "func_detail");
    const auto spec = tr::specOf(e->raw);
    REQUIRE(spec);
    CHECK(*spec == tr::Spec{10, 6, 1, 4});
    REQUIRE(e->brushes.size() == 6);   // ceil(10/4) x ceil(6/4)
    std::set<std::string> ids;
    for (const auto& b : e->brushes) {
        ids.insert(b->brush.id);
        CHECK_FALSE(b->brush.checkStructure().has_value());
        CHECK(closedManifold(b->brush));
        CHECK(signedVolume(b->brush) > 0);   // outward wound
        for (const auto& f : b->brush.faces) CHECK((f.loop.size() == 3 || f.loop.size() >= 4));
    }
    CHECK(ids.size() == 6);
}

TEST_CASE("terrain: an edge tile holds the leftover cells") {
    const auto last = tile(make(), 2, 1);   // 2 x 2 cells
    CHECK(last->brush.vertices.size() == 9 + 8);
    CHECK(last->brush.faces.size() == 2 * 4 + 8 + 1);
}

TEST_CASE("terrain: the volume of a flat terrain is area x thickness; the top faces up, walls and underside are caulk") {
    const Version v = make();
    double vol = 0;
    for (const auto& b : terrainNode(v)->brushes) vol += signedVolume(b->brush);
    CHECK(vol == doctest::Approx(10 * 6 * 4));
    const auto b = tile(v, 0, 0);
    for (const auto& f : b->brush.faces) CHECK(f.texture == (f.id < 2 * 16 ? "dev/grass" : "common/caulk"));
}

TEST_CASE("terrain: smooth edges join the top triangles only, so the whole surface shades as one") {
    const auto b = tile(make(), 0, 0);
    std::set<uint32_t> top;
    for (const auto& f : b->brush.faces)
        if (f.id < 32) top.insert(f.id);
    for (const auto& [a, c] : b->brush.smoothEdges) {
        CHECK(top.count(a) == 1);
        CHECK(top.count(c) == 1);
    }
    // 16 cells -> 32 triangles: 3 edges each, every interior edge is shared once; the border has 16 edges
    CHECK(b->brush.smoothEdges.size() == static_cast<size_t>((32 * 3 - 16) / 2));
}

TEST_CASE("terrain: sizes the Core and the editor would choke on are refused") {
    std::string err;
    auto p = params();
    p.cell = 0.01;
    CHECK_FALSE(tr::gridFor(p, err));
    p = params();
    p.sizeX = 4000;
    CHECK_FALSE(tr::gridFor(p, err));
    CHECK(err.find("Too many cells") == 0);
    p = params();
    p.thickness = 0;
    CHECK_FALSE(tr::gridFor(p, err));
    p = params();
    p.tile = 1;
    CHECK_FALSE(tr::gridFor(p, err));
    p = params();
    p.sizeX = 0;
    CHECK_FALSE(tr::gridFor(p, err));
    p = params();
    p.sizeX = p.sizeZ = 256;
    const auto ok = tr::gridFor(p, err);
    REQUIRE(ok);
    CHECK(ok->cols == 256);
    // a refused size leaves the document alone
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    doc::VersionEditor ed(r.version);
    p = params();
    p.sizeX = 5000;
    CHECK_FALSE(tr::addTerrain(ed, p, "t", bid, err));
    CHECK(ed.finish() == r.version);
}

TEST_CASE("terrain: terrainOf reads the heights back from the tiles") {
    const Version v = make();
    const auto f = hfOf(v);
    CHECK(f->spec == tr::Spec{10, 6, 1, 4});
    CHECK(f->ox == -5);
    CHECK(f->oz == -3);
    CHECK(f->base == -2);
    REQUIRE(f->h.size() == 11 * 7);
    CHECK(std::all_of(f->h.begin(), f->h.end(), [](double y) { return y == 2; }));
    CHECK(tr::terrainOf(terrainNode(v)) == f);   // cached
}

TEST_CASE("terrain: brush weight is full inside and drops smoothly to nothing at the radius") {
    CHECK(tr::brushWeight(0, 4, 0.5) == 1);
    CHECK(tr::brushWeight(2, 4, 0.5) == 1);
    CHECK(tr::brushWeight(4, 4, 0.5) == 0);
    CHECK(tr::brushWeight(3, 4, 0.5) == doctest::Approx(0.5));
    CHECK(tr::brushWeight(5, 4, 0.5) == 0);
    CHECK(tr::brushWeight(1, 4, 0) == 1);
    CHECK(tr::brushWeight(3.99, 4, 0) == 1);   // a hard edge
    CHECK(tr::brushWeight(0, 4, 1) == 1);
    CHECK(tr::brushWeight(2, 4, 1) == doctest::Approx(0.5));
}

TEST_CASE("terrain: raise adds strength x 0.5 m under the brush; lower and invert reverse it; it never digs through the bottom") {
    const tr::BrushParams P{2, 1, 0};
    tr::Heightfield f = *hfOf(make());
    const auto r = tr::stamp(f, tr::Tool::Raise, f.ox + 5, f.oz + 3, P);
    REQUIRE(r);
    CHECK(f.at(5, 3) == doctest::Approx(2.5));
    CHECK(f.at(4, 3) == doctest::Approx(2.5));
    CHECK(f.at(7, 3) == 2);   // exactly at the radius: untouched
    CHECK(f.at(0, 0) == 2);
    CHECK(*r == tr::PointRect{4, 6, 2, 4});   // the rim (distance 2) has weight 0
    tr::stamp(f, tr::Tool::Lower, f.ox + 5, f.oz + 3, P);
    CHECK(f.at(5, 3) == doctest::Approx(2));
    tr::StampOptions inv;
    inv.invert = true;
    tr::stamp(f, tr::Tool::Raise, f.ox + 5, f.oz + 3, P, inv);
    CHECK(f.at(5, 3) == doctest::Approx(1.5));
    tr::Heightfield g = *hfOf(make());
    for (int k = 0; k < 40; ++k) tr::stamp(g, tr::Tool::Lower, g.ox + 5, g.oz + 3, P);
    CHECK(g.at(5, 3) == doctest::Approx(g.base + 0.25));
}

TEST_CASE("terrain: smooth pulls a spike toward its neighbours; flatten pulls toward the target; a brush outside the grid clips") {
    tr::Heightfield f = *hfOf(make());
    f.h[3 * 11 + 5] = 4;   // a spike of 2 m
    tr::stamp(f, tr::Tool::Smooth, f.ox + 5, f.oz + 3, tr::BrushParams{1.5, 1, 0});
    CHECK(f.at(5, 3) == doctest::Approx(2 + 2.0 / 9).epsilon(1e-4));
    CHECK(f.at(5, 3) < 4);
    tr::Heightfield g = *hfOf(make());
    const tr::BrushParams P{2, 1, 0};
    tr::stamp(g, tr::Tool::Raise, g.ox + 5, g.oz + 3, P);
    tr::StampOptions to2;
    to2.target = 2;
    tr::stamp(g, tr::Tool::Flatten, g.ox + 5, g.oz + 3, tr::BrushParams{2, 0.5, 0}, to2);
    CHECK(g.at(5, 3) == doctest::Approx(2.25));
    tr::stamp(g, tr::Tool::Flatten, g.ox + 5, g.oz + 3, tr::BrushParams{2, 1, 0}, to2);
    CHECK(g.at(5, 3) == doctest::Approx(2));
    tr::Heightfield h = *hfOf(make());
    CHECK_FALSE(tr::stamp(h, tr::Tool::Raise, h.ox - 100, h.oz, P));
    const auto r = tr::stamp(h, tr::Tool::Raise, h.ox, h.oz, P);
    REQUIRE(r);
    CHECK(r->gx0 == 0);
    CHECK(r->gz0 == 0);
}

TEST_CASE("terrain: tilesOfRect, a point on a tile border belongs to both tiles") {
    const tr::Spec s = hfOf(make())->spec;   // tile 4: 3 across
    const auto sorted = [](std::vector<int> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted(tr::tilesOfRect(s, {4, 4, 1, 1})) == std::vector<int>{0, 1});
    CHECK(sorted(tr::tilesOfRect(s, {4, 4, 4, 4})) == std::vector<int>{0, 1, 3, 4});
    CHECK(tr::tilesOfRect(s, {10, 10, 6, 6}) == std::vector<int>{5});   // the far corner is in the last tile only
    CHECK(tr::tilesOfRect(s, {0, 0, 0, 0}) == std::vector<int>{0});
}

TEST_CASE("terrain: a stroke changes only the tiles it touched and the result is still a valid terrain") {
    const Version v = make();
    const tr::BrushParams P{2, 0.4, 0.5};
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Raise, P);
    s.moveTo(-5 + 1, -3 + 1);   // inside tile (0, 0), clear of every border
    const Version next = commitStroke(v, s);
    REQUIRE(next != v);
    CHECK(changedTiles(v, next) == 1);
    CHECK(tile(next, 1, 0) == tile(v, 1, 0));
    CHECK(tr::reconcileTerrain(v, next) == next);
    // the same content after a save and a reload is checked again and kept
    const doc::LoadResult back = doc::loadDocumentText(doc::serializeDocument(*next));
    REQUIRE(back.version);
    CHECK(tr::reconcileTerrain(nullptr, back.version) == back.version);
    CHECK(tr::terrainOf(terrainNode(back.version)));
    for (const auto& b : terrainNode(next)->brushes) {
        CHECK_FALSE(b->brush.checkStructure().has_value());
        CHECK(closedManifold(b->brush));
    }
    CHECK(maxOf(hfOf(next)->h) > 2);
}

TEST_CASE("terrain: a brush over a tile border moves the shared edge of both tiles the same way") {
    const Version v = make();
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Raise, tr::BrushParams{2, 0.4, 0.5});
    s.moveTo(-5 + 4, -3 + 2);   // on the border between tile (0,0) and (1,0)
    const Version next = s.document(v);
    const auto f = hfOf(next);   // terrainOf is null when a seam does not close
    CHECK(maxOf(f->h) > 2);
    CHECK(closedManifold(tile(next, 0, 0)->brush));
    CHECK(closedManifold(tile(next, 1, 0)->brush));
}

TEST_CASE("terrain: the stamps follow the path at a fixed spacing, many small moves = one long move") {
    const Version v = make();
    const tr::BrushParams P{2, 0.4, 0.5};
    tr::Stroke a("terrain1", hfOf(v), tr::Tool::Raise, P), b("terrain1", hfOf(v), tr::Tool::Raise, P);
    a.moveTo(-3, 0);
    a.moveTo(3, 0);
    b.moveTo(-3, 0);
    for (int k = 0; k <= 60; ++k) b.moveTo(-3 + k * 0.1, 0);
    b.moveTo(3, 0);
    double worst = 0;
    for (size_t k = 0; k < a.heights().h.size(); ++k) worst = std::max(worst, std::abs(a.heights().h[k] - b.heights().h[k]));
    CHECK(worst < 1e-3);
    CHECK(maxOf(a.heights().h) > 2.3);
}

TEST_CASE("terrain: flatten aims at the height under the first stamp, or the fixed height") {
    const Version v = make();
    tr::Stroke up("terrain1", hfOf(v), tr::Tool::Raise, tr::BrushParams{3, 1, 0});
    up.moveTo(-5 + 2, -3 + 3);
    const Version hill = up.document(v);
    tr::StrokeOptions fixed;
    fixed.flattenHeight = 2;
    tr::Stroke flat("terrain1", hfOf(hill), tr::Tool::Flatten, tr::BrushParams{6, 1, 0}, fixed);
    flat.moveTo(-5 + 5, -3 + 3);
    CHECK(maxOf(flat.heights().h) == doctest::Approx(2));
    tr::Stroke sample("terrain1", hfOf(hill), tr::Tool::Flatten, tr::BrushParams{1, 1, 0});
    const double top = tr::heightAt(*hfOf(hill), -5 + 2, -3 + 3);
    sample.moveTo(-5 + 2, -3 + 3);
    CHECK(tr::heightAt(sample.heights(), -5 + 2, -3 + 3) == doctest::Approx(top));   // flattening at the top keeps the top
}

TEST_CASE("terrain: paint changes the texture of the top faces under the brush and no geometry") {
    const Version v = make();
    tr::StrokeOptions o;
    o.texture = "dev/rock";
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Paint, tr::BrushParams{2, 0.4, 0.5}, o);
    s.moveTo(-5 + 1.5, -3 + 1.5);
    const Version next = s.document(v);
    const auto e = terrainNode(next);
    const tr::Spec sp = *tr::specOf(e->raw);
    CHECK(tr::cellTexture(*e, sp, 1, 1) == "dev/rock");
    CHECK(tr::cellTexture(*e, sp, 8, 5) == "dev/grass");
    const auto before = tile(v, 0, 0), after = tile(next, 0, 0);
    CHECK(after->brush.vertices.size() == before->brush.vertices.size());
    for (size_t i = 0; i < before->brush.vertices.size(); ++i) CHECK(after->brush.vertices[i].y == before->brush.vertices[i].y);
    CHECK(std::count_if(after->brush.faces.begin(), after->brush.faces.end(), [](const core::BrushFace& f) { return f.texture == "dev/rock"; }) > 0);
    for (const auto& f : after->brush.faces)
        if (f.id >= 32) CHECK(f.texture == "common/caulk");
    CHECK(maxOf(hfOf(next)->h) == 2);
}

TEST_CASE("terrain: a move that adds no stamp keeps what the stroke already did; nothing happens before the first stamp") {
    const Version v = make();
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Raise, tr::BrushParams{2, 0.4, 0.5});
    CHECK(s.document(v) == v);
    s.moveTo(-3, 0);
    const Version first = s.document(v);
    s.moveTo(-3.05, 0);   // far less than the spacing: no new stamp
    const Version again = s.document(v);
    CHECK(again == first);
    CHECK(first != v);
}

TEST_CASE("terrain: moving the whole entity keeps the terrain, the grid is read from the tiles") {
    const Version v = make();
    const auto moved = tools::transformSelections(v, {doc::SelRef::entity("terrain1")}, tools::translateBy({3, 1, -2}));
    REQUIRE(moved);
    CHECK(tr::reconcileTerrain(v, moved) == moved);
    const auto f = hfOf(moved);
    CHECK(f->ox == doctest::Approx(-2));
    CHECK(f->oz == doctest::Approx(-5));
    CHECK(f->base == doctest::Approx(-1));
    CHECK(std::all_of(f->h.begin(), f->h.end(), [](double y) { return std::abs(y - 3) < 1e-9; }));
}

TEST_CASE("terrain: a vertex moved sideways turns the terrain into ordinary brushes, a lifted inner vertex is a sculpt") {
    const Version v = make();
    core::Brush b = tile(v, 0, 0)->brush;
    b.vertices[6].x += 0.3;
    const Version bad = withBrush(v, b);
    const Version r = tr::reconcileTerrain(v, bad);
    CHECK(r != bad);
    CHECK_FALSE(r->entities[static_cast<size_t>(r->findEntity("terrain1"))]->raw.contains("terrain"));
    for (const auto& x : terrainNode(r)->brushes) CHECK_FALSE(x->raw.contains("terrainTile"));
    CHECK(terrainNode(r)->brushes.size() == 6);   // the geometry is all still there
    core::Brush inner = tile(v, 0, 0)->brush;
    inner.vertices[6].y += 1;
    const Version lifted = withBrush(v, inner);
    CHECK(tr::reconcileTerrain(v, lifted) == lifted);
    CHECK(tr::terrainOf(terrainNode(lifted)));
    core::Brush border = tile(v, 0, 0)->brush;
    border.vertices[4].y += 1;   // grid point (4, 0): shared with tile (1, 0), which did not move
    const Version cracked = withBrush(v, border);
    CHECK_FALSE(terrainNode(tr::reconcileTerrain(v, cracked))->raw.contains("terrain"));
}

TEST_CASE("terrain: one tile moved alone, one deleted, or a face split ends it; editing a texture does not") {
    const Version v = make();
    core::Brush b = tile(v, 1, 1)->brush;
    for (auto& p : b.vertices) p.x += 1;
    CHECK_FALSE(terrainNode(tr::reconcileTerrain(v, withBrush(v, b)))->raw.contains("terrain"));
    doc::VersionEditor ed(v);
    REQUIRE(ed.removeBrush("terrain1", tile(v, 1, 1)->brush.id));
    const Version deleted = ed.finish();
    REQUIRE(deleted);
    CHECK_FALSE(terrainNode(tr::reconcileTerrain(v, deleted))->raw.contains("terrain"));
    core::Brush t = tile(v, 1, 1)->brush;
    for (auto& f : t.faces) f.texture = "dev/red";
    const Version textured = withBrush(v, t);
    CHECK(tr::reconcileTerrain(v, textured) == textured);
}

TEST_CASE("terrain: a tile that ends up in another entity loses its mark") {
    const Version v = make();
    doc::VersionEditor ed(v);
    core::Entity other;
    other.id = "other";
    other.classname = "func_detail";
    REQUIRE(ed.addEntity(other));
    const auto first = tile(v, 0, 0);
    REQUIRE(ed.removeBrush("terrain1", first->brush.id));
    REQUIRE(ed.addBrush("other", first->brush, first->raw));
    REQUIRE(ed.setEntityKey("terrain1", "terrain", Json()));
    const Version next = ed.finish();
    REQUIRE(next);
    REQUIRE(terrainNode(next, "other")->brushes[0]->raw.contains("terrainTile"));
    const Version r = tr::reconcileTerrain(nullptr, next);
    CHECK_FALSE(terrainNode(r, "other")->brushes[0]->raw.contains("terrainTile"));
}

TEST_CASE("terrain: a duplicated terrain with new ids and the same content is still a terrain") {
    const Version v = make();
    doc::VersionEditor ed(v);
    core::Entity copy = terrainNode(v)->typed;
    copy.id = "copy";
    REQUIRE(ed.addEntity(copy));
    for (const auto& b : terrainNode(v)->brushes) {
        core::Brush nb = b->brush;
        nb.id += "c";
        REQUIRE(ed.addBrush("copy", nb, b->raw));
    }
    REQUIRE(ed.setEntityKey("copy", "terrain", terrainNode(v)->raw["terrain"]));
    const Version next = ed.finish();
    REQUIRE(next);
    CHECK(tr::reconcileTerrain(v, next) == next);
    CHECK(tr::terrainOf(terrainNode(next, "copy")));
}

TEST_CASE("terrain: a damaged record is not a terrain") {
    const Version v = make();
    doc::VersionEditor ed(v);
    REQUIRE(ed.setEntityKey("terrain1", "terrain", Json{{"cols", 3}, {"rows", 3}, {"cell", 1}, {"tile", 4}}));
    const Version small = ed.finish();
    CHECK_FALSE(tr::terrainOf(terrainNode(small)));
    CHECK_FALSE(tr::specOf(Json{{"terrain", Json{{"cols", "x"}}}}));
    CHECK_FALSE(tr::specOf(Json{{"id", "x"}}));
    CHECK(tr::specOf(Json{{"terrain", Json{{"cols", 2}, {"rows", 2}, {"cell", 0.5}, {"tile", 2}}}}));
}

namespace {
Version bumpy() {
    auto p = params();
    p.sizeX = p.sizeZ = 12;
    p.tile = 5;
    const Version v = make(p);
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Raise, tr::BrushParams{3, 1, 0.3});
    s.moveTo(-5, -3);
    s.moveTo(-1, 0);
    s.moveTo(1, 2);
    return s.document(v);
}
} // namespace

TEST_CASE("terrain: heightAt agrees with the triangles of the tiles, a ray down hits the same height") {
    const auto f = hfOf(bumpy());
    CHECK(tr::heightAt(*f, f->ox + 3, f->oz + 4) == doctest::Approx(f->at(3, 4)));
    const double pts[5][2] = {{-4.3, -2.2}, {0.1, 0.7}, {1.5, 2.5}, {-0.2, 3.9}, {3.33, -1.1}};
    for (const auto& p : pts) {
        const auto hit = tr::rayTerrain(*f, {p[0], 50, p[1]}, {0, -1, 0});
        REQUIRE(hit);
        CHECK(hit->point[1] == doctest::Approx(tr::heightAt(*f, p[0], p[1])).epsilon(1e-6));
    }
}

TEST_CASE("terrain: rays at an angle find the nearest triangle; misses are misses") {
    const auto f = hfOf(bumpy());
    const tools::terrain::V3 origin{f->ox - 5, 30, f->oz - 5}, tgt{f->ox + 6, tr::heightAt(*f, f->ox + 6, f->oz + 6), f->oz + 6};
    tools::terrain::V3 dir{tgt[0] - origin[0], tgt[1] - origin[1], tgt[2] - origin[2]};
    const double l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (double& d : dir) d /= l;
    const auto hit = tr::rayTerrain(*f, origin, dir);
    REQUIRE(hit);
    CHECK(hit->point[0] == doctest::Approx(tgt[0]).epsilon(1e-5));
    CHECK(hit->point[2] == doctest::Approx(tgt[2]).epsilon(1e-5));
    CHECK_FALSE(tr::rayTerrain(*f, {f->ox - 5, 30, f->oz - 5}, {0, 1, 0}));
    CHECK_FALSE(tr::rayTerrain(*f, {f->ox + 100, 30, f->oz}, {0, -1, 0}));
}

TEST_CASE("terrain: the cursor circle lies on the ground") {
    const auto f = hfOf(bumpy());
    const auto lines = tr::circleOnTerrain(*f, f->ox + 6, f->oz + 6, 3, 16, 0.05);
    REQUIRE(lines.size() == 16 * 6);
    for (size_t k = 0; k < lines.size(); k += 3) CHECK(lines[k + 1] == doctest::Approx(tr::heightAt(*f, lines[k], lines[k + 2]) + 0.05));
}

TEST_CASE("terrain: a 256 x 256 terrain is created and sculpted quickly, a stroke leaves almost all tiles untouched") {
    auto p = params();
    p.sizeX = p.sizeZ = 256;
    p.tile = 16;
    p.thickness = 10;
    p.origin = {0, 0, 0};
    const auto t0 = std::chrono::steady_clock::now();
    const Version v = make(p);
    const double created = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(terrainNode(v)->brushes.size() == 256);
    const auto t1 = std::chrono::steady_clock::now();
    tr::Stroke s("terrain1", hfOf(v), tr::Tool::Raise, tr::BrushParams{8, 0.5, 0.7});
    s.moveTo(100, 100);
    for (int x = 100; x < 140; x += 2) s.moveTo(x, 100);
    const Version next = s.document(v);
    const double stroked = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    CHECK(changedTiles(v, next) < 20);
    MESSAGE("256x256: create " << created << " ms, stroke " << stroked << " ms, read back " << tr::tilesAcross(hfOf(next)->spec) << " tiles across");
    CHECK(created < 6000);
    CHECK(stroked < 1500);
    CHECK(tr::tilesAcross(hfOf(next)->spec) == 16);
}
