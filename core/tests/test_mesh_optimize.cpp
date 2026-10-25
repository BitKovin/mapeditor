#include "core/mesh_optimize.h"
#include <doctest.h>
#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <functional>
#include <random>
#include <set>

using namespace core;

namespace {

using rbsp::DrawVert;

constexpr double kCell = 10;  // Quake units

DrawVert mk(double x, double y, double z, double su, double sv) {
    DrawVert v{};
    v.xyz[0] = float(x); v.xyz[1] = float(y); v.xyz[2] = float(z);
    v.st[0] = float(su); v.st[1] = float(sv);
    v.normal[2] = 1;
    return v;
}

// A grid of nx * ny cells as a triangle soup (three own vertices per triangle, as the compiler stages a relief). Height and texture coordinate are functions
// of the position; the normal stays +Z (the tests that matter for the normal build their own).
MeshItem soupGrid(int nx, int ny, const std::function<double(double, double)>& height, const std::function<std::array<double, 2>(double, double, int side)>& st = {},
                  double originX = 0) {
    MeshItem it;
    it.optimize = true;
    auto stAt = [&](double x, double y, int side) -> std::array<double, 2> { return st ? st(x, y, side) : std::array<double, 2>{x / 256, y / 256}; };
    auto vert = [&](double x, double y, int side) { const auto t = stAt(x, y, side); return mk(x, y, height(x, y), t[0], t[1]); };
    auto tri = [&](double ax, double ay, double bx, double by, double cx, double cy, int side) {
        const int32_t base = static_cast<int32_t>(it.verts.size());
        it.verts.push_back(vert(ax, ay, side)); it.verts.push_back(vert(bx, by, side)); it.verts.push_back(vert(cx, cy, side));
        for (int k = 0; k < 3; ++k) it.indexes.push_back(base + k);
    };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const double x0 = originX + i * kCell, x1 = x0 + kCell, y0 = j * kCell, y1 = y0 + kCell;
            const int side = (st && x0 + kCell * 0.5 > 80) ? 1 : 0;
            tri(x0, y0, x1, y0, x1, y1, side);
            tri(x0, y0, x1, y1, x0, y1, side);
        }
    return it;
}

double area(const MeshItem& it) {
    double a = 0;
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3) {
        const DrawVert &p = it.verts[size_t(it.indexes[t])], &q = it.verts[size_t(it.indexes[t + 1])], &r = it.verts[size_t(it.indexes[t + 2])];
        a += 0.5 * ((double(q.xyz[0]) - p.xyz[0]) * (double(r.xyz[1]) - p.xyz[1]) - (double(q.xyz[1]) - p.xyz[1]) * (double(r.xyz[0]) - p.xyz[0]));
    }
    return a;
}

// The surface height of `it` at (x, y) (interpolated in the triangle that contains the point); NaN when none does.
double heightAt(const MeshItem& it, double x, double y) {
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3) {
        const DrawVert &p = it.verts[size_t(it.indexes[t])], &q = it.verts[size_t(it.indexes[t + 1])], &r = it.verts[size_t(it.indexes[t + 2])];
        const double d = (double(q.xyz[1]) - r.xyz[1]) * (double(p.xyz[0]) - r.xyz[0]) + (double(r.xyz[0]) - q.xyz[0]) * (double(p.xyz[1]) - r.xyz[1]);
        if (std::fabs(d) < 1e-12) continue;
        const double a = ((double(q.xyz[1]) - r.xyz[1]) * (x - r.xyz[0]) + (double(r.xyz[0]) - q.xyz[0]) * (y - r.xyz[1])) / d;
        const double b = ((double(r.xyz[1]) - p.xyz[1]) * (x - r.xyz[0]) + (double(p.xyz[0]) - r.xyz[0]) * (y - r.xyz[1])) / d;
        const double c = 1 - a - b;
        const double e = -1e-4;
        if (a < e || b < e || c < e) continue;
        return a * p.xyz[2] + b * q.xyz[2] + c * r.xyz[2];
    }
    return std::nan("");
}

// the largest distance of the original's vertices from the reduced surface (height direction)
double maxDeviation(const MeshItem& original, const MeshItem& reduced) {
    double worst = 0;
    for (const DrawVert& v : original.verts) {
        const double h = heightAt(reduced, v.xyz[0], v.xyz[1]);
        if (std::isnan(h)) return 1e9;  // a hole
        worst = std::max(worst, std::fabs(h - v.xyz[2]));
    }
    return worst;
}

double hashNoise(double x, double y) {
    const double s = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
    return s - std::floor(s) - 0.5;  // -0.5 .. 0.5
}

size_t vertsAt(const MeshItem& it, double x) {
    size_t n = 0;
    for (const DrawVert& v : it.verts) n += std::fabs(double(v.xyz[0]) - x) < 1e-4;
    return n;
}

}  // namespace

TEST_CASE("mesh optimize: a flat triangle soup collapses to its outline") {
    MeshItem it = soupGrid(16, 16, [](double, double) { return 0.0; });
    const size_t before = it.verts.size();
    const double a0 = area(it);
    REQUIRE(before == 16 * 16 * 6);
    std::vector<MeshItem> items{it};
    MeshOptimizeStats st;
    optimizeSurfaces(items, MeshOptimizeParams{}, &st);
    const MeshItem& r = items[0];
    CHECK(r.verts.size() <= 6);
    CHECK(r.indexes.size() / 3 <= 4);
    CHECK(area(r) == doctest::Approx(a0).epsilon(1e-6));
    CHECK(st.weldedVertices > 0);
    CHECK(st.surfaces == 1);
    // the texture coordinate stays the plane's affine map
    for (const DrawVert& v : r.verts) {
        CHECK(v.st[0] == doctest::Approx(v.xyz[0] / 256).epsilon(1e-4));
        CHECK(v.st[1] == doctest::Approx(v.xyz[1] / 256).epsilon(1e-4));
    }
}

TEST_CASE("mesh optimize: vertices that differ by float noise are one point") {
    MeshItem it = soupGrid(8, 8, [](double, double) { return 0.0; });
    // every use of a corner gets its own small error, far below the weld distance
    for (size_t i = 0; i < it.verts.size(); ++i) {
        it.verts[i].xyz[0] += float(hashNoise(double(i), 1) * 2e-4);
        it.verts[i].xyz[1] += float(hashNoise(double(i), 2) * 2e-4);
        it.verts[i].xyz[2] += float(hashNoise(double(i), 3) * 2e-4);
    }
    std::vector<MeshItem> items{it};
    MeshOptimizeStats st;
    optimizeSurfaces(items, MeshOptimizeParams{}, &st);
    CHECK(st.weldedVertices > 0);
    CHECK(items[0].verts.size() <= 6);
    CHECK(area(items[0]) == doctest::Approx(80.0 * 80.0).epsilon(1e-3));
}

TEST_CASE("mesh optimize: a bump on a plane keeps its height, the plane around it goes") {
    auto h = [](double x, double y) { return (x >= 60 && x <= 100 && y >= 60 && y <= 100) ? 8.0 : 0.0; };
    MeshItem it = soupGrid(16, 16, h);
    std::vector<MeshItem> items{it};
    optimizeSurfaces(items, MeshOptimizeParams{});
    const MeshItem& r = items[0];
    float top = -1e9f;
    for (const DrawVert& v : r.verts) top = std::max(top, v.xyz[2]);
    CHECK(top == doctest::Approx(8.0f));
    CHECK(r.verts.size() < 120);
    CHECK(maxDeviation(it, r) < 1.0 / 16);
}

TEST_CASE("mesh optimize: a texture seam survives: no triangle crosses it") {
    // the texture coordinate jumps by 5 repeats at x = 80: the vertices on the seam exist twice, one set for each side
    auto st = [](double x, double y, int side) { return std::array<double, 2>{x / 256 + (side ? 5.0 : 0.0), y / 256}; };
    MeshItem it = soupGrid(16, 16, [](double, double) { return 0.0; }, st);
    std::vector<MeshItem> items{it};
    optimizeSurfaces(items, MeshOptimizeParams{});
    const MeshItem& r = items[0];
    CHECK(r.verts.size() < it.verts.size() / 10);
    for (size_t t = 0; t + 2 < r.indexes.size(); t += 3) {
        bool left = false, right = false;
        double su = 0;
        for (int k = 0; k < 3; ++k) {
            const DrawVert& v = r.verts[size_t(r.indexes[t + size_t(k)])];
            left = left || v.xyz[0] < 80 - 1e-3;
            right = right || v.xyz[0] > 80 + 1e-3;
            su += v.st[0];
        }
        CHECK(!(left && right));
    }
    for (const DrawVert& v : r.verts) {
        // the texture coordinate of every kept vertex is the one of its side (a seam vertex is told by its coordinate)
        const double plain = v.xyz[0] / 256, shifted = plain + 5.0;
        const bool onSeam = std::fabs(double(v.xyz[0]) - 80) < 1e-3;
        if (!onSeam) CHECK(v.st[0] == doctest::Approx(v.xyz[0] < 80 ? plain : shifted).epsilon(1e-3));
        else CHECK((std::fabs(v.st[0] - plain) < 1e-3 || std::fabs(v.st[0] - shifted) < 1e-3));
    }
}

TEST_CASE("mesh optimize: outline vertices that another surface also has stay; the others on a straight edge go") {
    auto edgeCount = [](bool withNeighbour, MeshOptimizeStats* stats) {
        std::vector<MeshItem> items;
        items.push_back(soupGrid(8, 8, [](double, double) { return 0.0; }));  // x 0..80
        if (withNeighbour) {  // a polygon on its +X side with a corner at every 10 units of the shared edge
            MeshItem n;
            n.optimize = false;
            for (int k = 0; k <= 8; ++k) { n.verts.push_back(mk(80, k * kCell, 0, 80 / 256.0, k * kCell / 256)); n.verts.push_back(mk(100, k * kCell, 0, 100 / 256.0, k * kCell / 256)); }
            for (int k = 0; k < 8; ++k) {
                const int32_t a = k * 2, b = k * 2 + 1, c = k * 2 + 2, d = k * 2 + 3;
                for (int32_t i : {a, b, d, a, d, c}) n.indexes.push_back(i);
            }
            items.push_back(n);
        }
        optimizeSurfaces(items, MeshOptimizeParams{}, stats);
        return vertsAt(items[0], 80);
    };
    MeshOptimizeStats alone, shared;
    CHECK(edgeCount(false, &alone) == 2);   // the two corners
    CHECK(edgeCount(true, &shared) == 9);   // every vertex the neighbour has at that edge
    CHECK(shared.sharedVertices >= 9);
}

TEST_CASE("mesh optimize: detail below the tolerance is thinned, detail above it stays") {
    auto run = [](double amplitude) {
        MeshItem it = soupGrid(24, 24, [amplitude](double x, double y) { return amplitude * hashNoise(x, y); });
        const size_t unique = 25 * 25;
        std::vector<MeshItem> items{it};
        MeshOptimizeParams p;  // tolerance 1/32
        optimizeSurfaces(items, p);
        return std::make_tuple(items[0].verts.size(), unique, maxDeviation(it, items[0]));
    };
    const auto [fine, unique1, dev1] = run(0.01);   // a hundredth of a unit: invisible
    CHECK(fine < unique1 / 5);
    CHECK(dev1 < 1.0 / 16);
    const auto [rough, unique2, dev2] = run(2.0);   // two units: a real relief
    CHECK(rough > unique2 / 2);
    CHECK(dev2 < 2.0 / 16);
}

TEST_CASE("mesh optimize: a surface that is not marked is left as it is") {
    MeshItem it = soupGrid(4, 4, [](double, double) { return 0.0; });
    it.optimize = false;
    std::vector<MeshItem> items{it};
    optimizeSurfaces(items, MeshOptimizeParams{});
    CHECK(items[0].verts.size() == it.verts.size());
    CHECK(items[0].indexes == it.indexes);
}

TEST_CASE("mesh optimize: triangles the welding closes are dropped") {
    MeshItem it;
    it.optimize = true;
    // a real triangle, and one whose corners are the same point to within float noise
    it.verts = {mk(0, 0, 0, 0, 0), mk(10, 0, 0, 0.04, 0), mk(0, 10, 0, 0, 0.04), mk(20, 20, 0, 0.08, 0.08), mk(20.0005, 20, 0, 0.08, 0.08), mk(20, 20.0005, 0, 0.08, 0.08)};
    it.indexes = {0, 1, 2, 3, 4, 5};
    std::vector<MeshItem> items{it};
    MeshOptimizeStats st;
    optimizeSurfaces(items, MeshOptimizeParams{}, &st);
    CHECK(items[0].indexes.size() == 3);
    CHECK(items[0].verts.size() == 3);
    CHECK(st.removedTriangles == 1);
}

TEST_CASE("mesh optimize: slots, vertex lit colour and normals take part in what is flat") {
    // a plane whose lightmap coordinate has a kink along x = 40: the vertices there carry a lighting change and stay
    MeshItem it = soupGrid(8, 8, [](double, double) { return 0.0; });
    it.lightmapSlots = 1;
    for (DrawVert& v : it.verts) {
        v.lightmap[0][0] = float(v.xyz[0] < 40 ? v.xyz[0] / 800 : 0.05 + (v.xyz[0] - 40) / 80);  // slope changes at x = 40
        v.lightmap[0][1] = float(v.xyz[1] / 800);
    }
    std::vector<MeshItem> items{it};
    optimizeSurfaces(items, MeshOptimizeParams{});
    for (size_t t = 0; t + 2 < items[0].indexes.size(); t += 3) {  // no triangle spans the crease: it would blend the two slopes
        bool left = false, right = false;
        for (size_t k = 0; k < 3; ++k) {
            const double x = items[0].verts[size_t(items[0].indexes[t + k])].xyz[0];
            left = left || x < 40 - 1e-3;
            right = right || x > 40 + 1e-3;
        }
        CHECK(!(left && right));
    }
    CHECK(items[0].verts.size() < it.verts.size() / 10);  // the rest is not
    for (const DrawVert& v : items[0].verts) CHECK(v.lightmap[0][0] == doctest::Approx(v.xyz[0] < 40 ? v.xyz[0] / 800 : 0.05 + (v.xyz[0] - 40) / 80).epsilon(2e-3));
}

// The engine builds its physics mesh from the exported triangles and refuses the ones without area. The lossless collapse of a straight outline edge
// leaves such a triangle (three collinear vertices) behind, so the optimizer has to drop them itself.
TEST_CASE("mesh optimize: no triangle without area is left (the physics mesh refuses them)") {
    const MeshOptimizeParams P;
    for (double jitter : {1e-5, 1e-4, 1e-3, 3.9e-3})
        for (int seed = 0; seed < 40; ++seed) {
            std::mt19937 rng(static_cast<unsigned>(seed));
            std::uniform_real_distribution<double> U(-1, 1);
            MeshItem it = soupGrid(8, 8, [](double, double) { return 0.0; });
            for (DrawVert& v : it.verts) { v.xyz[0] += float(U(rng) * jitter); v.xyz[1] += float(U(rng) * jitter); }  // the same corner computed twice differs by noise
            const double a0 = area(it);
            std::vector<MeshItem> items{it};
            optimizeSurfaces(items, P);
            const MeshItem& r = items[0];
            REQUIRE(!r.indexes.empty());
            for (size_t t = 0; t + 2 < r.indexes.size(); t += 3) {
                const float *a = r.verts[size_t(r.indexes[t])].xyz, *b = r.verts[size_t(r.indexes[t + 1])].xyz, *c = r.verts[size_t(r.indexes[t + 2])].xyz;
                const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1], uz = double(b[2]) - a[2], vx = double(c[0]) - a[0], vy = double(c[1]) - a[1], vz = double(c[2]) - a[2];
                const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
                INFO("jitter " << jitter << " seed " << seed << " triangle " << t / 3);
                CHECK(std::sqrt(cx * cx + cy * cy + cz * cz) >= P.minTriangleCross);
            }
            for (int32_t i : r.indexes) CHECK((i >= 0 && size_t(i) < r.verts.size()));
            CHECK(area(r) == doctest::Approx(a0).epsilon(1e-3));  // the dropped triangles had no area: the surface is the same
        }
}

// The clipping of a relief at the repeat borders and the welding leave slivers with one very short edge (a vertex computed twice, 0.01 units apart). The
// engine's physics mesh welds close vertices, so such a sliver becomes a triangle with two equal corners and the mesh is refused. The edge is collapsed
// instead (not the triangle dropped: that would leave a crack).
TEST_CASE("mesh optimize: no edge shorter than minEdge is left, and the surface stays closed") {
    const MeshOptimizeParams P;
    for (double gap : {0.006, 0.01, 0.02, 0.03})
        for (int seed = 0; seed < 20; ++seed) {
            std::mt19937 rng(static_cast<unsigned>(seed));
            std::uniform_real_distribution<double> U(0, 1);
            MeshItem it = soupGrid(6, 6, [](double x, double y) { return 0.3 * std::sin(x * 0.2) * std::cos(y * 0.2); });  // a relief: not collapsible by planarity
            // every use of some corners is moved a little way along the surface: the neighbouring cells now disagree by `gap`
            for (DrawVert& v : it.verts)
                if (U(rng) < 0.3) { const double a = U(rng) * 6.2831853; v.xyz[0] += float(std::cos(a) * gap); v.xyz[1] += float(std::sin(a) * gap); }
            const double a0 = area(it);
            std::vector<MeshItem> items{it};
            MeshOptimizeStats st;
            optimizeSurfaces(items, P, &st);
            const MeshItem& r = items[0];
            REQUIRE(!r.indexes.empty());
            for (size_t t = 0; t + 2 < r.indexes.size(); t += 3) {
                INFO("gap " << gap << " seed " << seed << " triangle " << t / 3);
                const float* c3[3] = {r.verts[size_t(r.indexes[t])].xyz, r.verts[size_t(r.indexes[t + 1])].xyz, r.verts[size_t(r.indexes[t + 2])].xyz};
                for (int k = 0; k < 3; ++k) {
                    const float *a = c3[k], *b = c3[(k + 1) % 3];
                    const double d = std::sqrt(std::pow(double(a[0]) - b[0], 2) + std::pow(double(a[1]) - b[1], 2) + std::pow(double(a[2]) - b[2], 2));
                    CHECK(d >= P.minEdge);
                }
            }
            CHECK(area(r) == doctest::Approx(a0).epsilon(2e-2));  // collapsing moves a corner by less than minEdge: the area barely changes
        }
}

TEST_CASE("mesh optimize: a vertex another surface shares does not move when a short edge collapses") {
    MeshOptimizeParams P;
    P.tolerance = 0;  // only the exact merges and the short edges
    MeshItem a = soupGrid(4, 4, [](double x, double) { return 0.4 * std::sin(x * 0.3); });
    // one corner pair 0.015 apart on the right outline x = 40
    for (DrawVert& v : a.verts) if (std::fabs(v.xyz[0] - 40) < 1e-4 && std::fabs(v.xyz[1] - 20) < 1e-4 && (&v - a.verts.data()) % 2) v.xyz[1] += 0.015f;
    MeshItem b;  // a neighbouring polygon (not optimized) that has a vertex at the first of them
    b.optimize = false;
    b.verts = {mk(40, 20, 0.4 * std::sin(40 * 0.3), 0, 0), mk(60, 20, 0, 0, 0), mk(60, 40, 0, 0, 0)};
    b.indexes = {0, 1, 2};
    std::vector<MeshItem> items{a, b};
    optimizeSurfaces(items, P);
    bool kept = false;
    for (const DrawVert& v : items[0].verts) kept |= std::fabs(double(v.xyz[0]) - 40) < 1e-6 && std::fabs(double(v.xyz[1]) - 20) < 1e-6;
    CHECK(kept);
}
