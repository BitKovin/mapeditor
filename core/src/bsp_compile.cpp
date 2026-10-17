// The rBSP compiler (REQUIREMENTS В§25, DESIGN В§12).
#include "core/bsp_compile.h"
#include "core/csg.h"
#include "core/parallel.h"
#include "core/raytrace.h"
#include "core/smoothing.h"
#include "core/textures.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>

namespace core {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isWorldClass(const std::string& c) { return c == "worldspawn" || c == "func_group" || c == "func_detail"; }

std::string entityKey(const std::string& instance, const std::string& id) { return instance + '\x1f' + id; }

void copyName(char* dst, size_t n, const std::string& s) {
    std::memset(dst, 0, n);
    std::memcpy(dst, s.data(), std::min(s.size(), n - 1));
}

void put3(float* dst, const Vec3& v) { dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z); }

struct Bounds {
    Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    bool valid() const { return mn.x <= mx.x; }
    void add(const Vec3& p) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    void add(const Bounds& b) { if (b.valid()) { add(b.mn); add(b.mx); } }
};

// Planes are stored in pairs: i ^ 1 is i flipped (Quake 3's convention).
struct PlaneSet {
    std::vector<rbsp::Plane> list;
    std::map<std::array<long long, 4>, int> index;
    int add(const Vec3& n, double d) {
        auto key = [](const Vec3& n, double d) {
            return std::array<long long, 4>{std::llround(n.x * 1e6), std::llround(n.y * 1e6), std::llround(n.z * 1e6), std::llround(d * 1e3)};
        };
        const auto k = key(n, d);
        if (const auto it = index.find(k); it != index.end()) return it->second;
        const int i = static_cast<int>(list.size());
        rbsp::Plane p{}, q{};
        put3(p.normal, n); p.dist = float(d);
        put3(q.normal, n * -1.0); q.dist = float(-d);
        list.push_back(p); list.push_back(q);
        index[k] = i;
        index[key(n * -1.0, -d)] = i + 1;
        return i;
    }
};

struct ShaderSet {
    std::vector<rbsp::Shader> list;
    std::map<std::string, int> index;
    std::string root;
    // One entry per (texture, contents, surface flags): the same texture on a trigger brush and on a wall are two shaders, as in Quake 3.
    int add(const std::string& texture, uint32_t contents, uint32_t surface) {
        const std::string key = texture + '' + std::to_string(contents) + '' + std::to_string(surface);
        if (const auto it = index.find(key); it != index.end()) return it->second;
        rbsp::Shader s{};
        copyName(s.name, sizeof(s.name), root.empty() ? texture : root + "/" + texture);
        s.contentFlags = static_cast<int32_t>(contents);
        s.surfaceFlags = static_cast<int32_t>(surface);
        index[key] = static_cast<int>(list.size());
        list.push_back(s);
        return index[key];
    }
};

std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.6g", std::fabs(v) < 5e-7 ? 0.0 : v);
    return b;
}

// Deterministic random numbers (vis sample points and ray rotations): a hash of (cell, sample, k).
double hash01(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h >> 8) * (1.0 / 16777216.0);
}

// Shelf packing of rectangles into square pages: (page, x, y) per rectangle.
struct Placement { int page = 0, x = 0, y = 0; };
std::vector<Placement> shelfPack(const std::vector<std::pair<int, int>>& sizes, int pageSize, int& pages) {
    std::vector<size_t> order(sizes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sizes[a].second > sizes[b].second; });
    std::vector<Placement> out(sizes.size());
    pages = 0;
    int x = 0, y = 0, shelf = 0;
    bool open = false;
    for (size_t i : order) {
        const int w = std::min(sizes[i].first, pageSize), h = std::min(sizes[i].second, pageSize);
        if (!open) { pages = 1; open = true; }
        if (x + w > pageSize) { x = 0; y += shelf; shelf = 0; }
        if (y + h > pageSize) { ++pages; x = 0; y = 0; shelf = 0; }
        out[i] = {pages - 1, x, y};
        x += w;
        shelf = std::max(shelf, h);
    }
    return out;
}

}  // namespace

BspOutput compileBsp(const BspInput& in, const ProgressFn& progress) {
    BspOutput res;
    auto stage = [&](const char* name, double f) { if (progress) progress(name, f); };
    const BspSettings& cfg = in.settings;
    stage("geometry", 0);

    // ---- models: the world (worldspawn, func_group, func_detail) and one submodel per other brush entity -------------------------------------
    std::map<std::string, int> modelOf;            // entity key -> model
    std::vector<const FlattenedEntity*> modelEntity = {nullptr};
    const FlattenedEntity* worldspawn = nullptr;
    for (const auto& fe : in.entities) {
        const std::string key = entityKey(instanceString(fe.instancePath), fe.entity.id);
        if (fe.entity.classname == "worldspawn" && !worldspawn) worldspawn = &fe;
        if (isWorldClass(fe.entity.classname)) modelOf[key] = 0;
        else if (!fe.entity.brushes.empty()) { modelOf[key] = static_cast<int>(modelEntity.size()); modelEntity.push_back(&fe); }
    }
    const size_t modelCount = modelEntity.size();

    ShaderSet shaders;
    shaders.root = cfg.textureRoot;
    PlaneSet planes;

    // ---- lightmap pages (DESIGN §12 "8 lightmaps, 4 UV sets"): slot 0 = the base pages as baked; slot k = the k-th style layer of every chart
    // that has one. The engine's vertex has 4 lightmap UV sets, so slots 1 and 2 are packed densely on their own (UV sets 1, 2) and slots 3..7
    // share ONE placement (UV set 3, computed for the charts with a 4th layer), each slot in its own pages.
    const LightmapLayout* layout = in.layout;
    const auto basePages = in.pages.find(0);
    const bool lit = layout && basePages != in.pages.end() && basePages->second.atlas > 0;
    const int atlas = lit ? basePages->second.atlas : 0;
    std::array<std::map<int, Placement>, rbsp::kStyles> placed;  // slot -> chart -> where its layer went (output page index)
    if (lit) {
        const LightmapPages& bp = basePages->second;
        for (int p = 0; p < bp.pageCount; ++p) res.lightmaps.push_back({atlas, bp.rgb[static_cast<size_t>(p)]});
        for (size_t c = 0; c < layout->charts.size(); ++c) placed[0][static_cast<int>(c)] = {layout->charts[c].page, layout->charts[c].x, layout->charts[c].y};
        size_t dropped = 0;
        for (const auto& ch : layout->charts) dropped += ch.styles.size() > static_cast<size_t>(rbsp::kStyles);
        if (dropped) res.warnings.push_back(std::to_string(dropped) + " lightmap chart(s) have more than 8 light styles; the rest are dropped");
        std::vector<Placement> shared;   // slots 3..7: placement per chart index (page relative to the shared set)
        std::vector<int> sharedCharts;
        for (int k = 1; k < rbsp::kStyles; ++k) {
            std::vector<int> charts;
            for (size_t c = 0; c < layout->charts.size(); ++c)
                if (layout->charts[c].styles.size() > static_cast<size_t>(k)) charts.push_back(static_cast<int>(c));
            if (charts.empty()) break;
            std::map<int, Placement> where;  // chart -> placement inside this slot's (relative) pages
            if (k <= 3) {
                std::vector<std::pair<int, int>> sizes;
                for (int c : charts) sizes.push_back({layout->charts[static_cast<size_t>(c)].w, layout->charts[static_cast<size_t>(c)].h});
                int pages = 0;
                const auto w = shelfPack(sizes, atlas, pages);
                for (size_t i = 0; i < charts.size(); ++i) where[charts[i]] = w[i];
                if (k == 3) { sharedCharts = charts; shared = w; }
            } else {
                for (size_t i = 0; i < sharedCharts.size(); ++i) where[sharedCharts[i]] = shared[i];  // the same positions as slot 3
            }
            std::map<int, int> outPage;  // relative page -> output page (only pages that hold a chart of this slot)
            for (int c : charts) {
                const auto& ch = layout->charts[static_cast<size_t>(c)];
                const Placement rel = where.at(c);
                auto op = outPage.find(rel.page);
                if (op == outPage.end()) {
                    op = outPage.emplace(rel.page, static_cast<int>(res.lightmaps.size())).first;
                    res.lightmaps.push_back({atlas, std::vector<float>(static_cast<size_t>(atlas) * atlas * 3, 0.0f)});
                }
                const Placement dst{op->second, rel.x, rel.y};
                placed[static_cast<size_t>(k)][c] = dst;
                const auto pit = in.pages.find(ch.styles[static_cast<size_t>(k)]);
                if (pit == in.pages.end()) continue;
                const LightmapPages& pg = pit->second;
                if (ch.page >= pg.pageCount || pg.atlas != atlas) continue;
                const auto& src = pg.rgb[static_cast<size_t>(ch.page)];
                auto& out = res.lightmaps[static_cast<size_t>(dst.page)].rgb;
                for (int y = 0; y < ch.h && dst.y + y < atlas && ch.y + y < atlas; ++y)
                    for (int x = 0; x < ch.w && dst.x + x < atlas && ch.x + x < atlas; ++x)
                        for (int q = 0; q < 3; ++q)
                            out[((static_cast<size_t>(dst.y + y)) * atlas + dst.x + x) * 3 + q] = src[((static_cast<size_t>(ch.y + y)) * atlas + ch.x + x) * 3 + q];
            }
        }
    }
    res.stats.lightmapPages = res.lightmaps.size();

    // ---- draw surfaces ------------------------------------------------------------------------------------------------------------------------
    std::vector<rbsp::DrawVert> verts;
    std::vector<int32_t> indexes;
    std::vector<rbsp::Surface> surfaces;
    std::vector<std::vector<int>> modelSurfaces(modelCount), modelDecals(modelCount);
    std::vector<Bounds> surfaceBounds;  // Quake units, per output surface (before reordering)
    std::vector<char> surfaceSky;       // ... and whether it is sky
    std::set<std::string> flatSky;      // sky textures that are not skyboxes (the engine draws them flat)
    std::vector<rbsp::Surface> unordered;
    std::vector<std::pair<size_t, size_t>> unorderedVerts, unorderedIdx;  // ranges into the staging arrays
    std::vector<rbsp::DrawVert> stageVerts;
    std::vector<int32_t> stageIdx;
    // A piece is dropped only when every part of it lies inside matter: pieces are not clipped against other additive brushes, so a floor piece that
    // runs under a wall is partly buried and partly seen (testing its centre alone opened holes beside walls). Samples on a grid of ~1/8 m over
    // the fan triangles, lifted off the surface; any sample in the open keeps the piece.
    auto buried = [&](const LightSurface& s) {
        SolidSet::Probe probe;
        const Vec3 lift = s.normal * 0.02;
        const Vec3& a = s.verts[0];
        for (size_t i = 1; i + 1 < s.verts.size(); ++i) {
            const Vec3 &b = s.verts[i], &c = s.verts[i + 1];
            const double len = std::max({(b - a).length(), (c - a).length(), (c - b).length()});
            const int steps = std::clamp(static_cast<int>(std::ceil(len / 0.125)), 1, 256);
            for (int u = 0; u <= steps; ++u)
                for (int v = 0; u + v <= steps; ++v) {
                    // pulled a hair towards the triangle's centre: edges shared with the wall's own face are not "in the open"
                    const double fu = (u + 1e-3) / (steps + 3e-3), fv = (v + 1e-3) / (steps + 3e-3);
                    const Vec3 p = a + (b - a) * fu + (c - a) * fv;
                    if (!in.solids->inside(p + lift, probe)) return false;
                }
        }
        return true;
    };
    struct FaceTexturing { bool plain = true; std::function<texplan::V2(const texplan::V3&)> uvAt; };
    std::map<std::pair<const Brush*, uint32_t>, FaceTexturing> faceTexturing;
    texplan::ReliefBudget reliefBudget;
    reliefBudget.left = 2000000;
    // One convex polygon (CCW seen from outside, editor space) -> a staged surface. `st` gives its texture coordinates, `lift` moves it off its
    // plane (Quake units, decals), the lightmap comes from the authored face (instance, entity, brush instance, brush, face) when lightmapped.
    struct Poly {
        int model = 0;
        std::string texture, classname, instance, entityId, brushInstance, brushId;
        uint32_t faceId = 0;
        Vec3 normal;
        const FaceShading* shading = nullptr;  // smooth edges: vertex normals vary across the face (REQUIREMENTS §33 H6)
        const std::vector<Vec3>* verts = nullptr;
        std::function<Vec2(size_t i, const Vec3& p)> st;
        double lift = 0;
        bool decal = false;
        // Triangle soup instead of the polygon (repeat modes, texture group layouts, reliefs: texplan.h), CCW seen from the front, 3 entries per triangle.
        // `verts` stays the authored piece (lightmap face lookup, plane); positions off the plane (a relief) take the lightmap of their footprint.
        std::vector<Vec3> triPos, triNrm;
        std::vector<Vec2> triSt;
    };
    auto emit = [&](const Poly& s) {
        rbsp::Surface out{};
        const bool sky = cfg.contents.isSky(s.texture);  // no lightmap, no contents, seen from every cluster (REQUIREMENTS §28 K4)
        if (sky && !isCubemapTexture(s.texture)) flatSky.insert(s.texture);
        const BspContentRules::Flags fl = cfg.contents.forFace(s.texture);
        out.shader = s.decal ? shaders.add(s.texture, 0, fl.surface | cfg.contents.decalFlag()) : shaders.add(s.texture, fl.contents, fl.surface);
        out.fog = -1;
        out.type = s.decal || !s.triPos.empty() ? rbsp::kSurfaceTriangles : rbsp::kSurfacePolygon;
        std::memset(out.lightmapStyles, rbsp::kNoStyle, sizeof(out.lightmapStyles));
        std::memset(out.vertexStyles, rbsp::kNoStyle, sizeof(out.vertexStyles));
        for (int k = 0; k < rbsp::kStyles; ++k) out.lightmapNum[k] = -1;
        put3(out.normal, toQuakeDir(s.normal));
        const std::vector<Vec3>& pv = *s.verts;

        const LightmapFace* lf = nullptr;
        const LightmapChart* chart = nullptr;
        if (lit && !sky && in.classes.isLightmapped(s.classname)) {
            lf = layout->findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, pv);
            if (lf && lf->chart >= 0) chart = &layout->charts[static_cast<size_t>(lf->chart)];
        }
        // per slot: texel offset from the base placement
        std::array<Vec2, rbsp::kStyles> shift{};
        int slots = 0;
        if (chart) {
            for (int k = 0; k < rbsp::kStyles && k < static_cast<int>(chart->styles.size()); ++k) {
                const auto cit = placed[static_cast<size_t>(k)].find(lf->chart);
                if (cit == placed[static_cast<size_t>(k)].end()) break;
                out.lightmapStyles[k] = static_cast<uint8_t>(chart->styles[static_cast<size_t>(k)]);
                out.lightmapNum[k] = cit->second.page;
                out.lightmapCorner[k][0] = cit->second.x;
                out.lightmapCorner[k][1] = cit->second.y;
                shift[static_cast<size_t>(k)] = {double(cit->second.x - chart->x), double(cit->second.y - chart->y)};
                slots = k + 1;
            }
            out.lightmapSize[0] = chart->w;
            out.lightmapSize[1] = chart->h;
            // the chart's world axes in texels per Quake unit, and the world point of texel (0, 0) on the face plane
            put3(out.lightmapVecs[0], toQuakeDir(lf->su) * (1.0 / rbsp::kUnitsPerMeter));
            put3(out.lightmapVecs[1], toQuakeDir(lf->sv) * (1.0 / rbsp::kUnitsPerMeter));
            const Vec3 n = s.normal, a = lf->su, b = lf->sv;
            const double rhs[3] = {-lf->so, -lf->to, n.dot(pv[0])};
            const double m[3][3] = {{a.x, a.y, a.z}, {b.x, b.y, b.z}, {n.x, n.y, n.z}};
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (std::fabs(det) > 1e-12) {
                double x[3];
                for (int col = 0; col < 3; ++col) {
                    double t[3][3];
                    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i][j] = j == col ? rhs[i] : m[i][j];
                    x[col] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                              t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
                }
                put3(out.lightmapOrigin, toQuake({x[0], x[1], x[2]}));
            }
        }

        const size_t vFirst = stageVerts.size(), iFirst = stageIdx.size();
        const Vec3 lift = toQuakeDir(s.normal) * s.lift;
        Bounds bb;
        if (!s.triPos.empty()) {
            const double planeD = s.normal.dot(pv[0]);
            auto vert = [&](size_t k) {
                const Vec3& p = s.triPos[k];
                rbsp::DrawVert dv{};
                const Vec3 q = toQuake(p) + lift;
                put3(dv.xyz, q);
                bb.add(q);
                dv.st[0] = float(s.triSt[k].u); dv.st[1] = float(s.triSt[k].v);
                if (chart) {
                    const Vec3 foot = p - s.normal * (s.normal.dot(p) - planeD);  // a relief point takes the lightmap of the flat point under it
                    const Vec2 uv = layout->uv(*lf, foot);
                    for (int k2 = 0; k2 < slots; ++k2) {
                        dv.lightmap[k2][0] = float(uv.u + shift[static_cast<size_t>(k2)].u / atlas);
                        dv.lightmap[k2][1] = float(uv.v + shift[static_cast<size_t>(k2)].v / atlas);
                    }
                }
                // a flat triangle of a smoothed face takes the smooth normal; a relief keeps its own
                const bool flat = (s.triNrm[k] - s.normal).length() < 1e-6;
                put3(dv.normal, toQuakeDir(s.shading && flat ? shadingNormalAt(*s.shading, p, s.normal) : s.triNrm[k]));
                std::memset(dv.color, 255, sizeof(dv.color));
                stageVerts.push_back(dv);
            };
            for (size_t t = 0; t + 2 < s.triPos.size(); t += 3) {
                const int32_t base = static_cast<int32_t>(stageVerts.size() - vFirst);
                vert(t); vert(t + 2); vert(t + 1);  // Quake's windings are clockwise seen from the front
                stageIdx.push_back(base); stageIdx.push_back(base + 1); stageIdx.push_back(base + 2);
            }
            out.numVerts = static_cast<int32_t>(stageVerts.size() - vFirst);
            out.numIndexes = static_cast<int32_t>(stageIdx.size() - iFirst);
            (s.decal ? modelDecals : modelSurfaces)[static_cast<size_t>(s.model)].push_back(static_cast<int>(unordered.size()));
            unordered.push_back(out);
            unorderedVerts.push_back({vFirst, stageVerts.size() - vFirst});
            unorderedIdx.push_back({iFirst, stageIdx.size() - iFirst});
            surfaceBounds.push_back(bb);
            surfaceSky.push_back(sky);
            return;
        }
        const size_t n = pv.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t src = n - 1 - i;  // Quake's windings are clockwise seen from the front
            const Vec3& p = pv[src];
            rbsp::DrawVert dv{};
            const Vec3 q = toQuake(p) + lift;
            put3(dv.xyz, q);
            bb.add(q);
            const Vec2 st = s.st(src, p);
            dv.st[0] = float(st.u); dv.st[1] = float(st.v);
            if (chart) {
                const Vec2 uv = layout->uv(*lf, p);
                for (int k = 0; k < slots; ++k) {
                    dv.lightmap[k][0] = float(uv.u + shift[static_cast<size_t>(k)].u / atlas);
                    dv.lightmap[k][1] = float(uv.v + shift[static_cast<size_t>(k)].v / atlas);
                }
            }
            put3(dv.normal, toQuakeDir(s.shading ? shadingNormalAt(*s.shading, p, s.normal) : s.normal));
            std::memset(dv.color, 255, sizeof(dv.color));
            stageVerts.push_back(dv);
        }
        for (size_t i = 1; i + 1 < n; ++i) {
            stageIdx.push_back(0);
            stageIdx.push_back(static_cast<int32_t>(i));
            stageIdx.push_back(static_cast<int32_t>(i + 1));
        }
        out.numVerts = static_cast<int32_t>(n);
        out.numIndexes = static_cast<int32_t>(stageIdx.size() - iFirst);
        (s.decal ? modelDecals : modelSurfaces)[static_cast<size_t>(s.model)].push_back(static_cast<int>(unordered.size()));
        unordered.push_back(out);
        unorderedVerts.push_back({vFirst, n});
        unorderedIdx.push_back({iFirst, stageIdx.size() - iFirst});
        surfaceBounds.push_back(bb);
        surfaceSky.push_back(sky);
    };
    for (const LightSurface& s : in.surfaces) {
        const auto mit = modelOf.find(entityKey(s.instance, s.entityId));
        if (mit == modelOf.end() || s.verts.size() < 3) continue;
        // buried in a static solid: never seen (a moving entity keeps all its faces); sky faces are always kept
        if (mit->second == 0 && in.solids && !cfg.contents.isSky(s.texture) && buried(s)) { ++res.stats.droppedSurfaces; continue; }
        Poly p;
        p.model = mit->second;
        p.texture = s.texture; p.classname = s.classname; p.instance = s.instance; p.entityId = s.entityId;
        p.brushInstance = s.brushInstance; p.brushId = s.brushId; p.faceId = s.faceId;
        p.normal = s.normal;
        p.shading = s.shading;
        p.verts = &s.verts;
        // the texture as the editor lays it (texplan.h): the authored face's UV map; repeat modes, texture group layouts and reliefs as triangles
        const auto parent = in.parentFace ? in.parentFace(s.brushInstance, s.brushId, s.faceId) : std::pair<const Brush*, const BrushFace*>{nullptr, nullptr};
        if (parent.first && parent.second) {
            const auto key = std::make_pair(parent.first, parent.second->id);
            auto pit = faceTexturing.find(key);
            if (pit == faceTexturing.end()) {
                FaceTexturing ft;
                ft.plain = !texplan::planForFace(*parent.first, *parent.second, in.texInfo, in.texGroups) &&
                           !texplan::reliefOf(in.texInfo, parent.second->texture.empty() ? std::string("dev/grey") : parent.second->texture);
                ft.uvAt = texplan::faceUvAt(*parent.first, *parent.second, in.texInfo);
                pit = faceTexturing.emplace(key, std::move(ft)).first;
            }
            if (pit->second.plain) {
                const auto uvAt = pit->second.uvAt;
                p.st = [uvAt](size_t, const Vec3& q) { const texplan::V2 uv = uvAt(texplan::v3(q)); return Vec2{uv[0], uv[1]}; };
                emit(p);
                continue;
            }
            std::vector<texplan::V3> poly;
            for (const Vec3& v : s.verts) poly.push_back(texplan::v3(v));
            const auto tris = texplan::texturePolygon(poly, texplan::v3(s.normal), parent.first, parent.second, in.texInfo, in.texGroups, &reliefBudget);
            std::map<std::string, Poly> byTexture;  // one surface per texture the face samples
            for (const auto& t : tris) {
                Poly& q = byTexture[t.texture];
                if (q.triPos.empty()) { q = p; q.texture = t.texture; }
                for (int k = 0; k < 3; ++k) {
                    q.triPos.push_back(texplan::vec(t.pos[static_cast<size_t>(k)]));
                    q.triNrm.push_back(texplan::vec(t.normal[static_cast<size_t>(k)]));
                    q.triSt.push_back({t.uv[static_cast<size_t>(k)][0], t.uv[static_cast<size_t>(k)][1]});
                }
            }
            for (auto& [tex, q] : byTexture) emit(q);
            ++res.stats.laidOutSurfaces;
            continue;
        }
        p.st = [&s](size_t, const Vec3& q) { return albedoUvAt(s, q); };
        emit(p);
    }
    // decals: on the surface of the entity that owns their face, lifted off it, before every other surface of that model (REQUIREMENTS §26 F8)
    for (const DecalFragment& d : in.decals) {
        const auto mit = modelOf.find(entityKey(d.instance, d.entityId));
        if (mit == modelOf.end() || d.verts.size() < 3 || d.uv.size() != d.verts.size()) continue;
        Poly p;
        p.model = mit->second;
        p.texture = d.texture; p.classname = d.classname; p.instance = d.instance; p.entityId = d.entityId;
        p.brushInstance = d.brushInstance; p.brushId = d.brushId; p.faceId = d.faceId;
        p.normal = d.normal;
        p.verts = &d.verts;
        p.st = [&d](size_t i, const Vec3&) { return d.uv[i]; };
        p.lift = cfg.decalOffset;
        p.decal = true;
        emit(p);
        ++res.stats.decals;
    }
    // the automatic sky (REQUIREMENTS §28 K2 / K3): per box 6 inward-facing quads of the world (no lightmap, no contents, in every cluster)
    if (cfg.sky.mode != AutoSky::Off) {
        std::string tex = cfg.sky.texture;
        if (tex.empty()) { tex = "env/sky_cube"; res.warnings.push_back("auto sky: no skybox in env/ and no sky texture set; using env/sky_cube"); }
        const std::string worldId = worldspawn ? worldspawn->entity.id : std::string();
        for (const SkyBox& box : autoSkyBoxes(in.surfaces, cfg.sky.mode, cfg.sky.margin, cfg.contents)) {
            const double across = (box.mx - box.mn).length();
            if (across > cfg.engineFarPlane)
                res.warnings.push_back("auto sky: a sky box is " + std::to_string(static_cast<int>(across)) + " m across, beyond the engine's far plane (" +
                                       std::to_string(static_cast<int>(cfg.engineFarPlane)) + " m): its far corners are clipped in the game");
            const Vec3 c[2] = {box.mn, box.mx};
            auto comp = [](const Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; };
            for (int axis = 0; axis < 3; ++axis)
                for (int side = 0; side < 2; ++side) {
                    Vec3 n{0, 0, 0};
                    (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = side == 0 ? 1 : -1;  // the face at the min side looks towards +axis: inwards
                    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
                    std::vector<Vec3> quad;
                    for (int k = 0; k < 4; ++k) {
                        const int u = (k == 1 || k == 2) ? 1 : 0, v = k >= 2 ? 1 : 0;
                        double q[3];
                        q[axis] = comp(c[side], axis);
                        q[a1] = comp(c[u], a1);
                        q[a2] = comp(c[v], a2);
                        quad.push_back({q[0], q[1], q[2]});
                    }
                    if ((quad[1] - quad[0]).cross(quad[2] - quad[0]).dot(n) < 0) std::reverse(quad.begin(), quad.end());  // CCW seen from inside
                    Poly p;
                    p.model = 0;
                    p.texture = tex; p.classname = "worldspawn"; p.entityId = worldId;
                    p.normal = n;
                    p.verts = &quad;
                    p.st = [](size_t, const Vec3&) { return Vec2{0, 0}; };  // a skybox is sampled by direction
                    emit(p);
                    ++res.stats.skySurfaces;
                }
        }
    }
    for (const auto& t : flatSky) res.warnings.push_back("sky texture " + t + " is not a skybox (env/<name>_cube): the engine draws it as a flat texture");
    // emit model by model (a model's surfaces are one contiguous range): its decals first
    std::vector<Bounds> modelBounds(modelCount);
    std::vector<std::pair<int, int>> modelSurfRange(modelCount);
    std::vector<char> outSky;  // per output surface
    for (size_t m = 0; m < modelCount; ++m) {
        modelSurfRange[m].first = static_cast<int>(surfaces.size());
        std::vector<int> order = modelDecals[m];
        order.insert(order.end(), modelSurfaces[m].begin(), modelSurfaces[m].end());
        for (int u : order) {
            rbsp::Surface s = unordered[static_cast<size_t>(u)];
            s.firstVert = static_cast<int32_t>(verts.size());
            s.firstIndex = static_cast<int32_t>(indexes.size());
            const auto [vf, vn] = unorderedVerts[static_cast<size_t>(u)];
            const auto [xf, xn] = unorderedIdx[static_cast<size_t>(u)];
            verts.insert(verts.end(), stageVerts.begin() + static_cast<std::ptrdiff_t>(vf), stageVerts.begin() + static_cast<std::ptrdiff_t>(vf + vn));
            indexes.insert(indexes.end(), stageIdx.begin() + static_cast<std::ptrdiff_t>(xf), stageIdx.begin() + static_cast<std::ptrdiff_t>(xf + xn));
            surfaces.push_back(s);
            outSky.push_back(surfaceSky[static_cast<size_t>(u)]);
            modelBounds[m].add(surfaceBounds[static_cast<size_t>(u)]);
        }
        modelSurfRange[m].second = static_cast<int>(surfaces.size()) - modelSurfRange[m].first;
    }
    res.stats.surfaces = surfaces.size();
    stage("brushes", 0.15);

    // ---- collision brushes: convex pieces of the additive brushes, carved by the subtractors in their scope ---------------------------------
    struct Cutter { const Brush* brush; const FlattenedEntity* owner; std::string instance; std::vector<ConvexPiece> pieces; };
    std::vector<Cutter> cutters;
    for (const auto& fe : in.entities)
        for (const auto& b : fe.entity.brushes)
            if (b.op == BrushOp::Subtract) cutters.push_back({&b, &fe, instanceString(fe.instancePath), convexPieces(b)});
    std::vector<rbsp::Brush> brushes;
    std::vector<rbsp::BrushSide> sides;
    std::vector<Bounds> brushBounds;
    std::vector<std::pair<int, int>> modelBrushRange(modelCount);
    struct PieceOut { std::vector<HalfSpace> sides; Bounds bounds; std::string classname; };
    std::vector<std::vector<PieceOut>> modelPieces(modelCount);
    for (const auto& fe : in.entities) {
        const std::string inst = instanceString(fe.instancePath);
        const auto mit = modelOf.find(entityKey(inst, fe.entity.id));
        if (mit == modelOf.end()) continue;
        for (const auto& b : fe.entity.brushes) {
            if (b.op != BrushOp::Add) continue;
            std::vector<ConvexPiece> pieces = convexPieces(b);
            for (const auto& c : cutters) {
                if (!(c.instance == inst || c.instance.empty() || inst.empty())) continue;
                const bool same = c.owner == &fe;
                if (!carveScopeApplies(c.brush->carve, same, fe.entity) || !brushesMayOverlap(b, *c.brush)) continue;
                for (const auto& cut : c.pieces) {
                    std::vector<ConvexPiece> next;
                    for (const auto& p : pieces) for (auto& q : subtractConvex(p, cut)) next.push_back(std::move(q));
                    pieces = std::move(next);
                }
            }
            for (auto& p : pieces) {
                Bounds bb;
                for (const auto& w : convexWindings(p.sides)) for (const Vec3& v : w) bb.add(toQuake(v));
                modelPieces[static_cast<size_t>(mit->second)].push_back({std::move(p.sides), bb, fe.entity.classname});
            }
        }
    }
    for (size_t m = 0; m < modelCount; ++m) {
        modelBrushRange[m].first = static_cast<int>(brushes.size());
        for (const auto& [hs, bb, classname] : modelPieces[m]) {
            rbsp::Brush br{};
            br.firstSide = static_cast<int32_t>(sides.size());
            br.numSides = static_cast<int32_t>(hs.size());
            // contents: the entity class's rule when there is one, else what the sides' textures say (Quake 3: the union of the sides)
            uint32_t contents = 0, firstSurface = 0;
            std::string firstTex;
            for (const HalfSpace& h : hs) {
                const std::string tex = h.texture.empty() ? std::string("common/caulk") : h.texture;
                const BspContentRules::Flags fl = cfg.contents.forFace(tex);
                contents |= fl.contents;
                if (firstTex.empty()) { firstTex = tex; firstSurface = fl.surface; }
                sides.push_back({planes.add(toQuakeDir(h.normal), h.dist * rbsp::kUnitsPerMeter), shaders.add(tex, fl.contents, fl.surface)});
            }
            uint32_t classContents = 0;
            if (cfg.contents.forClass(classname, classContents)) contents = classContents;
            br.shader = hs.empty() ? 0 : shaders.add(firstTex, contents, firstSurface);
            brushes.push_back(br);
            brushBounds.push_back(bb);
            modelBounds[m].add(bb);
        }
        modelBrushRange[m].second = static_cast<int>(brushes.size()) - modelBrushRange[m].first;
    }
    res.stats.brushes = brushes.size();
    res.stats.models = modelCount;

    // ---- cells (the leafs of an axis aligned tree) and sampled visibility ---------------------------------------------------------------------
    stage("cells", 0.25);
    // the cells cover the world's geometry; the sky around it is not cut into cells (its surfaces go into every cluster's leafs)
    Bounds world;
    for (int sf = modelSurfRange[0].first; sf < modelSurfRange[0].first + modelSurfRange[0].second; ++sf)
        if (!outSky[static_cast<size_t>(sf)]) {
            const auto& su = surfaces[static_cast<size_t>(sf)];
            for (int v = 0; v < su.numVerts; ++v) { const float* p = verts[static_cast<size_t>(su.firstVert + v)].xyz; world.add(Vec3{p[0], p[1], p[2]}); }
        }
    for (int b = modelBrushRange[0].first; b < modelBrushRange[0].first + modelBrushRange[0].second; ++b) world.add(brushBounds[static_cast<size_t>(b)]);
    if (!world.valid()) { world.add(Vec3{-32, -32, -32}); world.add(Vec3{32, 32, 32}); }
    world.mn = world.mn - Vec3{1, 1, 1};
    world.mx = world.mx + Vec3{1, 1, 1};
    const Vec3 ext = world.mx - world.mn;
    double cell = std::max(cfg.visCell, 0.25) * rbsp::kUnitsPerMeter;
    auto dimsFor = [&](double c, std::array<int, 3>& d) {
        d = {std::max(1, int(std::ceil(ext.x / c))), std::max(1, int(std::ceil(ext.y / c))), std::max(1, int(std::ceil(ext.z / c)))};
        return static_cast<long long>(d[0]) * d[1] * d[2];
    };
    std::array<int, 3> dims{};
    const long long cap = static_cast<long long>(std::max(1, cfg.maxClusters)) * 32;
    while (dimsFor(cell, dims) > cap) cell *= 1.25;
    std::vector<char> solidCell;
    auto cellBox = [&](int i, int j, int k, Vec3& a, Vec3& b) {
        a = world.mn + Vec3{i * cell, j * cell, k * cell};
        b = a + Vec3{cell, cell, cell};
    };
    auto classify = [&]() {
        const size_t total = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
        solidCell.assign(total, 0);
        if (!in.solids) return total;
        parallelFor(total, 256, [&](size_t b0, size_t b1) {
            for (size_t ci = b0; ci < b1; ++ci) {
                const int i = static_cast<int>(ci % dims[0]), j = static_cast<int>((ci / dims[0]) % dims[1]), k = static_cast<int>(ci / (static_cast<size_t>(dims[0]) * dims[1]));
                Vec3 a, b;
                cellBox(i, j, k, a, b);
                bool all = true;
                for (int s = 0; s < 27 && all; ++s) {
                    const Vec3 q = a + Vec3{(1 + 2 * (s % 3)) / 6.0 * cell, (1 + 2 * ((s / 3) % 3)) / 6.0 * cell, (1 + 2 * (s / 9)) / 6.0 * cell};
                    all = in.solids->inside(fromQuake(q));
                }
                solidCell[ci] = all ? 1 : 0;
            }
        });
        size_t open = 0;
        for (char c : solidCell) open += c == 0;
        return open;
    };
    while (classify() > static_cast<size_t>(std::max(1, cfg.maxClusters))) { cell *= 1.25; dimsFor(cell, dims); }
    const size_t cellCount = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    std::vector<int> clusterOf(cellCount, -1);
    int clusters = 0;
    for (size_t ci = 0; ci < cellCount; ++ci) if (!solidCell[ci]) clusterOf[ci] = clusters++;
    res.stats.clusters = static_cast<size_t>(clusters);
    res.stats.leafs = cellCount;
    res.stats.visCell = cell / rbsp::kUnitsPerMeter;
    auto cellIndex = [&](int i, int j, int k) { return (static_cast<size_t>(k) * dims[1] + j) * dims[0] + i; };

    // kd tree over the cells
    std::vector<rbsp::Node> nodes;
    std::vector<rbsp::Leaf> leafs(cellCount);
    std::function<int32_t(std::array<int, 3>, std::array<int, 3>)> build = [&](std::array<int, 3> lo, std::array<int, 3> hi) -> int32_t {
        int axis = 0;
        for (int a = 1; a < 3; ++a) if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
        if (hi[axis] - lo[axis] <= 1) return -static_cast<int32_t>(cellIndex(lo[0], lo[1], lo[2])) - 1;
        const int mid = (lo[axis] + hi[axis]) / 2;
        const int32_t me = static_cast<int32_t>(nodes.size());
        nodes.push_back({});
        Vec3 n{0, 0, 0};
        (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = 1;
        const double d = (axis == 0 ? world.mn.x : axis == 1 ? world.mn.y : world.mn.z) + mid * cell;
        const int plane = planes.add(n, d);
        std::array<int, 3> lo2 = lo, hi2 = hi;
        lo2[axis] = mid;
        hi2[axis] = mid;
        const int32_t front = build(lo2, hi), back = build(lo, hi2);
        rbsp::Node& nd = nodes[static_cast<size_t>(me)];
        nd.plane = plane;
        nd.children[0] = front;
        nd.children[1] = back;
        for (int a = 0; a < 3; ++a) {
            const double base = a == 0 ? world.mn.x : a == 1 ? world.mn.y : world.mn.z;
            nd.mins[a] = static_cast<int32_t>(std::floor(base + lo[a] * cell));
            nd.maxs[a] = static_cast<int32_t>(std::ceil(base + hi[a] * cell));
        }
        return me;
    };
    const int32_t root = build({0, 0, 0}, dims);
    if (root < 0) {  // a single cell: Quake 3 needs a node, so split it in the middle (both sides are that cell's leaf duplicated)
        nodes.push_back({});
        nodes[0].plane = planes.add({1, 0, 0}, world.mn.x + cell * 0.5);
        nodes[0].children[0] = root;
        nodes[0].children[1] = root;
        for (int a = 0; a < 3; ++a) {
            nodes[0].mins[a] = static_cast<int32_t>(std::floor(a == 0 ? world.mn.x : a == 1 ? world.mn.y : world.mn.z));
            nodes[0].maxs[a] = static_cast<int32_t>(std::ceil(a == 0 ? world.mx.x : a == 1 ? world.mx.y : world.mx.z));
        }
    }

    // leaf contents: world surfaces and world brushes whose bounds overlap the cell
    std::vector<std::vector<int>> leafSurf(cellCount), leafBrush(cellCount);
    auto cellRange = [&](const Bounds& b, std::array<int, 3>& lo, std::array<int, 3>& hi) {
        const double mn[3] = {b.mn.x, b.mn.y, b.mn.z}, mx[3] = {b.mx.x, b.mx.y, b.mx.z}, base[3] = {world.mn.x, world.mn.y, world.mn.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::clamp(static_cast<int>(std::floor((mn[a] - 0.5 - base[a]) / cell)), 0, dims[a] - 1);
            hi[a] = std::clamp(static_cast<int>(std::floor((mx[a] + 0.5 - base[a]) / cell)), 0, dims[a] - 1);
        }
    };
    for (int s = modelSurfRange[0].first; s < modelSurfRange[0].first + modelSurfRange[0].second; ++s) {
        if (outSky[static_cast<size_t>(s)]) {  // sky: seen from every cluster
            for (size_t ci = 0; ci < cellCount; ++ci) if (clusterOf[ci] >= 0) leafSurf[ci].push_back(s);
            continue;
        }
        Bounds b;
        const auto& sf = surfaces[static_cast<size_t>(s)];
        for (int v = 0; v < sf.numVerts; ++v) { const float* p = verts[static_cast<size_t>(sf.firstVert + v)].xyz; b.add(Vec3{p[0], p[1], p[2]}); }
        std::array<int, 3> lo{}, hi{};
        cellRange(b, lo, hi);
        for (int k = lo[2]; k <= hi[2]; ++k) for (int j = lo[1]; j <= hi[1]; ++j) for (int i = lo[0]; i <= hi[0]; ++i) leafSurf[cellIndex(i, j, k)].push_back(s);
    }
    for (int b = modelBrushRange[0].first; b < modelBrushRange[0].first + modelBrushRange[0].second; ++b) {
        std::array<int, 3> lo{}, hi{};
        cellRange(brushBounds[static_cast<size_t>(b)], lo, hi);
        for (int k = lo[2]; k <= hi[2]; ++k) for (int j = lo[1]; j <= hi[1]; ++j) for (int i = lo[0]; i <= hi[0]; ++i) leafBrush[cellIndex(i, j, k)].push_back(b);
    }
    std::vector<int32_t> leafSurfaces, leafBrushes;
    for (int k = 0; k < dims[2]; ++k)
        for (int j = 0; j < dims[1]; ++j)
            for (int i = 0; i < dims[0]; ++i) {
                const size_t ci = cellIndex(i, j, k);
                rbsp::Leaf& l = leafs[ci];
                l.cluster = clusterOf[ci];
                l.area = 0;
                Vec3 a, b;
                cellBox(i, j, k, a, b);
                l.mins[0] = int32_t(std::floor(a.x)); l.mins[1] = int32_t(std::floor(a.y)); l.mins[2] = int32_t(std::floor(a.z));
                l.maxs[0] = int32_t(std::ceil(b.x)); l.maxs[1] = int32_t(std::ceil(b.y)); l.maxs[2] = int32_t(std::ceil(b.z));
                l.firstLeafSurface = static_cast<int32_t>(leafSurfaces.size());
                l.numLeafSurfaces = static_cast<int32_t>(leafSurf[ci].size());
                leafSurfaces.insert(leafSurfaces.end(), leafSurf[ci].begin(), leafSurf[ci].end());
                l.firstLeafBrush = static_cast<int32_t>(leafBrushes.size());
                l.numLeafBrushes = static_cast<int32_t>(leafBrush[ci].size());
                leafBrushes.insert(leafBrushes.end(), leafBrush[ci].begin(), leafBrush[ci].end());
            }

    // visibility: rays from sample points of every cluster cell; every cell a ray passes before it hits an occluder is visible
    stage("visibility", 0.35);
    const auto visT0 = std::chrono::steady_clock::now();
    const size_t words = (static_cast<size_t>(clusters) + 63) / 64;
    std::vector<uint64_t> vis(static_cast<size_t>(clusters) * words, 0);
    auto setBit = [&](std::vector<uint64_t>& v, int r, int c) { v[static_cast<size_t>(r) * words + static_cast<size_t>(c) / 64] |= uint64_t(1) << (c % 64); };
    auto getBit = [&](const std::vector<uint64_t>& v, int r, int c) { return (v[static_cast<size_t>(r) * words + static_cast<size_t>(c) / 64] >> (c % 64)) & 1; };
    if (cfg.visibility && clusters > 0) {
        std::vector<LightSurface> occ;
        for (const LightSurface& s : in.surfaces) {
            const auto mit = modelOf.find(entityKey(s.instance, s.entityId));
            if (mit == modelOf.end() || mit->second != 0) continue;                // only the static world occludes
            if (in.opaque && !in.opaque(s.texture)) continue;                      // glass, grates: see-through
            if (cfg.contents.isSky(s.texture)) continue;                            // sky: open
            occ.push_back(s);
            occ.back().classname = "worldspawn";
        }
        LightingClasses occClasses;
        occClasses.castShadows = {"worldspawn"};
        const OccluderMesh mesh = buildOccluders(occ, occClasses);
        const TriangleBvh bvh(mesh);
        const double diag = (world.mx - world.mn).length() / rbsp::kUnitsPerMeter;
        // ray directions: a Fibonacci sphere, rotated per sample point
        const int R = std::max(8, cfg.visRays), S = std::max(1, cfg.visSamples);
        std::vector<Vec3> dirs(static_cast<size_t>(R));
        for (int i = 0; i < R; ++i) {
            const double y = 1 - (i + 0.5) * 2.0 / R, r = std::sqrt(std::max(0.0, 1 - y * y)), phi = i * kPi * (3 - std::sqrt(5.0));
            dirs[static_cast<size_t>(i)] = {r * std::cos(phi), y, r * std::sin(phi)};
        }
        std::vector<size_t> cellOfCluster(static_cast<size_t>(clusters));
        for (size_t ci = 0; ci < cellCount; ++ci) if (clusterOf[ci] >= 0) cellOfCluster[static_cast<size_t>(clusterOf[ci])] = ci;
        std::atomic<size_t> doneCount{0};
        parallelFor(static_cast<size_t>(clusters), 1, [&](size_t c0, size_t c1) {
            for (size_t cl = c0; cl < c1; ++cl) {
                const size_t ci = cellOfCluster[cl];
                const int i = static_cast<int>(ci % dims[0]), j = static_cast<int>((ci / dims[0]) % dims[1]), k = static_cast<int>(ci / (static_cast<size_t>(dims[0]) * dims[1]));
                Vec3 a, b;
                cellBox(i, j, k, a, b);
                setBit(vis, static_cast<int>(cl), static_cast<int>(cl));
                int found = 0;
                for (int attempt = 0; attempt < S * 4 && found < S; ++attempt) {
                    const Vec3 q = a + Vec3{hash01(uint32_t(ci), uint32_t(attempt), 0) * cell, hash01(uint32_t(ci), uint32_t(attempt), 1) * cell,
                                            hash01(uint32_t(ci), uint32_t(attempt), 2) * cell};
                    const Vec3 o = fromQuake(q);
                    if (in.solids && in.solids->inside(o)) continue;
                    ++found;
                    // random rotation of the direction set (axis-angle)
                    const double u1 = hash01(uint32_t(ci), uint32_t(attempt), 3), u2 = hash01(uint32_t(ci), uint32_t(attempt), 4), ang = hash01(uint32_t(ci), uint32_t(attempt), 5) * 2 * kPi;
                    const double zz = 1 - 2 * u1, rr = std::sqrt(std::max(0.0, 1 - zz * zz));
                    const Vec3 ax{rr * std::cos(2 * kPi * u2), rr * std::sin(2 * kPi * u2), zz};
                    const double cs = std::cos(ang), sn = std::sin(ang);
                    for (const Vec3& d0 : dirs) {
                        const Vec3 d = d0 * cs + ax.cross(d0) * sn + ax * (ax.dot(d0) * (1 - cs));
                        const TriangleBvh::Hit h = bvh.closestHit(o, d, diag);
                        const double tEnd = h.t < 0 ? diag : h.t;
                        // 3D DDA through the cells (Quake space; the ray parameter t is the same in both spaces)
                        const Vec3 qo = toQuake(o), qd = toQuakeDir(d) * rbsp::kUnitsPerMeter;
                        int cx = std::clamp(static_cast<int>(std::floor((qo.x - world.mn.x) / cell)), 0, dims[0] - 1);
                        int cy = std::clamp(static_cast<int>(std::floor((qo.y - world.mn.y) / cell)), 0, dims[1] - 1);
                        int cz = std::clamp(static_cast<int>(std::floor((qo.z - world.mn.z) / cell)), 0, dims[2] - 1);
                        const double od[3] = {qd.x, qd.y, qd.z}, oo[3] = {qo.x, qo.y, qo.z}, base[3] = {world.mn.x, world.mn.y, world.mn.z};
                        int cc[3] = {cx, cy, cz}, step[3];
                        double tMax[3], tDelta[3];
                        for (int ax3 = 0; ax3 < 3; ++ax3) {
                            if (std::fabs(od[ax3]) < 1e-12) { step[ax3] = 0; tMax[ax3] = 1e300; tDelta[ax3] = 1e300; continue; }
                            step[ax3] = od[ax3] > 0 ? 1 : -1;
                            const double edge = base[ax3] + (cc[ax3] + (od[ax3] > 0 ? 1 : 0)) * cell;
                            tMax[ax3] = (edge - oo[ax3]) / od[ax3];
                            tDelta[ax3] = cell / std::fabs(od[ax3]);
                        }
                        for (;;) {
                            const int target = clusterOf[cellIndex(cc[0], cc[1], cc[2])];
                            if (target >= 0) setBit(vis, static_cast<int>(cl), target);
                            const int m = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
                            if (tMax[m] > tEnd) break;
                            cc[m] += step[m];
                            if (cc[m] < 0 || cc[m] >= dims[m]) break;
                            tMax[m] += tDelta[m];
                        }
                    }
                }
                const size_t done = ++doneCount;
                if (progress && cl == c0) progress("visibility", 0.35 + 0.5 * double(done) / clusters);
            }
        });
        // symmetric (if A sees B, B sees A), then dilated by one cell (a camera at a cell border sees what the neighbour sees)
        for (int r = 0; r < clusters; ++r)
            for (int c = 0; c < clusters; ++c)
                if (getBit(vis, r, c)) setBit(vis, c, r);
        std::vector<uint64_t> dil = vis;
        for (size_t ci = 0; ci < cellCount; ++ci) {
            const int r = clusterOf[ci];
            if (r < 0) continue;
            const int i = static_cast<int>(ci % dims[0]), j = static_cast<int>((ci / dims[0]) % dims[1]), k = static_cast<int>(ci / (static_cast<size_t>(dims[0]) * dims[1]));
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = i + dx, y = j + dy, z = k + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= dims[0] || y >= dims[1] || z >= dims[2]) continue;
                        const int nb = clusterOf[cellIndex(x, y, z)];
                        if (nb < 0) continue;
                        for (size_t w = 0; w < words; ++w) dil[static_cast<size_t>(r) * words + w] |= vis[static_cast<size_t>(nb) * words + w];
                    }
        }
        vis = std::move(dil);
    } else {
        std::fill(vis.begin(), vis.end(), ~uint64_t(0));
    }
    res.stats.visSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - visT0).count();
    stage("light grid", 0.88);

    // ---- light grid ---------------------------------------------------------------------------------------------------------------------------
    std::vector<rbsp::GridPoint> palette;
    std::vector<uint8_t> lightArray;
    // the grid has no validity flag and the engine interpolates the 8 corners as they are: in-wall probes take their neighbours' light
    std::map<int, LightvolGrid> lightvol = in.lightvol;
    for (auto& [st, grid] : lightvol) fillInvalidProbes(grid);
    if (const auto g0 = lightvol.find(0); g0 != lightvol.end() && g0->second.count() > 0) {
        const LightvolGrid& g = g0->second;
        rbsp::LightGridHeader hdr{};
        put3(hdr.origin, toQuake(g.origin + Vec3{0, 0, (g.nz - 1) * g.cell}));
        hdr.cellSize[0] = hdr.cellSize[1] = hdr.cellSize[2] = float(g.cell * rbsp::kUnitsPerMeter);
        hdr.dims[0] = g.nx; hdr.dims[1] = g.nz; hdr.dims[2] = g.ny;  // Quake x, y, z = editor x, -z, y
        lightArray.resize(sizeof(hdr) + g.count() * 2);
        std::memcpy(lightArray.data(), &hdr, sizeof(hdr));
        std::map<std::string, uint16_t> seen;
        bool overflow = false;
        for (int ez = 0; ez < g.nz; ++ez)
            for (int ey = 0; ey < g.ny; ++ey)
                for (int ex = 0; ex < g.nx; ++ex) {
                    const size_t ei = g.index(ex, ey, ez);
                    rbsp::GridPoint gp{};
                    std::memset(gp.styles, rbsp::kNoStyle, sizeof(gp.styles));
                    gp.styles[0] = 0;
                    // slots: style 0, then the brightest other styles
                    std::vector<std::pair<double, int>> rank;
                    for (const auto& [st, grid] : lightvol) {
                        if (st == 0 || grid.count() != g.count()) continue;
                        const Probe& p = grid.probes[ei];
                        if (!p.valid) continue;
                        const double lum = p.ambient.x + p.ambient.y + p.ambient.z + p.directed.x + p.directed.y + p.directed.z;
                        if (lum > 1e-6) rank.push_back({lum, st});
                    }
                    std::sort(rank.begin(), rank.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    std::vector<std::pair<int, const Probe*>> slots = {{0, &g.probes[ei]}};
                    for (const auto& [lum, st] : rank) if (slots.size() < rbsp::kStyles) slots.push_back({st, &lightvol.at(st).probes[ei]});
                    Vec3 dir, dirW;
                    for (size_t k = 0; k < slots.size(); ++k) {
                        const Probe& p = *slots[k].second;
                        gp.styles[k] = static_cast<uint8_t>(slots[k].first);
                        if (!p.valid) continue;
                        const Vec3 a = p.ambient, d = p.directed;
                        gp.ambient[k][0] = lightvolToByte(a.x); gp.ambient[k][1] = lightvolToByte(a.y); gp.ambient[k][2] = lightvolToByte(a.z);
                        gp.directed[k][0] = lightvolToByte(d.x); gp.directed[k][1] = lightvolToByte(d.y); gp.directed[k][2] = lightvolToByte(d.z);
                        dirW = dirW + p.dir * (d.x + d.y + d.z + 1e-9);
                    }
                    dir = dirW.length() > 1e-12 ? dirW.normalized() : Vec3{0, 1, 0};
                    const Vec3 qd = toQuakeDir(dir);
                    const double lat = std::acos(std::clamp(qd.z, -1.0, 1.0)) / kPi * 255.0;
                    double lng = std::atan2(qd.y, qd.x) / (2 * kPi);
                    if (lng < 0) lng += 1;
                    gp.latLong[0] = static_cast<uint8_t>(std::clamp(std::lround(lat), 0L, 255L));
                    gp.latLong[1] = static_cast<uint8_t>(std::lround(lng * 256.0) & 255);
                    const std::string key(reinterpret_cast<const char*>(&gp), sizeof(gp));
                    auto it = seen.find(key);
                    uint16_t idx;
                    if (it != seen.end()) idx = it->second;
                    else if (palette.size() < 65536) { idx = static_cast<uint16_t>(palette.size()); seen[key] = idx; palette.push_back(gp); }
                    else { idx = 0; overflow = true; }
                    const size_t qi = (static_cast<size_t>(ey) * g.nz + (g.nz - 1 - ez)) * g.nx + ex;  // z * (nx * ny) + y * nx + x in Quake axes
                    std::memcpy(lightArray.data() + sizeof(hdr) + qi * 2, &idx, 2);
                }
        if (overflow) res.warnings.push_back("light grid: more than 65536 different points, the rest use the first");
        res.stats.gridPoints = g.count();
        res.stats.palette = palette.size();
    }

    // ---- entities -----------------------------------------------------------------------------------------------------------------------------
    stage("entities", 0.92);
    std::string ents;
    auto kv = [&](const std::string& k, const std::string& v) {
        std::string vv = v;
        std::replace(vv.begin(), vv.end(), '"', '\'');
        ents += "\"" + k + "\" \"" + vv + "\"\n";
    };
    auto writeEntity = [&](const FlattenedEntity& fe, int model) {
        const Entity& e = fe.entity;
        ents += "{\n";
        kv("classname", e.classname);
        const bool turned = e.angles.x != 0 || e.angles.y != 0 || e.angles.z != 0;
        const bool world = &fe == worldspawn;
        // every keyvalue goes out (references are already resolved to names); the ones written from the entity's own fields below win
        std::vector<std::string> keys;
        for (const auto& [k, v] : e.keyvalues)
            if (k != "classname" && k != "model" && (world || (k != "origin" && k != "angles" && !(k == "angle" && turned)))) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) kv(k, e.keyvalues.at(k));
        if (model > 0) kv("model", "*" + std::to_string(model));
        // Point AND brush entities: origin and angles in the engine's own space (metres, Y up; pitch yaw roll in degrees) = the editor's (REQUIREMENTS
        // §26 F5, §34 B1). A brush entity's model stays in world space; its origin is the pivot it turns about (a rotating door), its angles how it faces.
        if (!world) {
            kv("origin", fmt(e.origin.x) + " " + fmt(e.origin.y) + " " + fmt(e.origin.z));
            if (turned) kv("angles", fmt(e.angles.x) + " " + fmt(e.angles.y) + " " + fmt(e.angles.z));
        }
        if (&fe == worldspawn) {
            if (const auto g0 = in.lightvol.find(0); g0 != in.lightvol.end() && g0->second.count() > 0) {
                const std::string c = fmt(g0->second.cell * rbsp::kUnitsPerMeter);
                kv("gridsize", c + " " + c + " " + c);
            }
        }
        ents += "}\n";
    };
    if (worldspawn) writeEntity(*worldspawn, 0);
    else ents += "{\n\"classname\" \"worldspawn\"\n}\n";
    for (const auto& fe : in.entities) {
        if (&fe == worldspawn || isWorldClass(fe.entity.classname)) continue;
        const auto mit = modelOf.find(entityKey(instanceString(fe.instancePath), fe.entity.id));
        writeEntity(fe, mit != modelOf.end() ? mit->second : (fe.entity.brushes.empty() ? -1 : -2));
    }

    // ---- models -------------------------------------------------------------------------------------------------------------------------------
    std::vector<rbsp::Model> models(modelCount);
    for (size_t m = 0; m < modelCount; ++m) {
        Bounds b = modelBounds[m];
        if (!b.valid()) { b.add(Vec3{0, 0, 0}); }
        put3(models[m].mins, b.mn);
        put3(models[m].maxs, b.mx);
        models[m].firstSurface = modelSurfRange[m].first;
        models[m].numSurfaces = modelSurfRange[m].second;
        models[m].firstBrush = modelBrushRange[m].first;
        models[m].numBrushes = modelBrushRange[m].second;
    }

    // ---- visibility lump ----------------------------------------------------------------------------------------------------------------------
    std::vector<uint8_t> visLump;
    {
        const int32_t bytesPer = (clusters + 7) / 8;
        visLump.resize(8 + static_cast<size_t>(clusters) * bytesPer, 0);
        std::memcpy(visLump.data(), &clusters, 4);
        std::memcpy(visLump.data() + 4, &bytesPer, 4);
        for (int r = 0; r < clusters; ++r)
            for (int c = 0; c < clusters; ++c)
                if (getBit(vis, r, c)) visLump[8 + static_cast<size_t>(r) * bytesPer + c / 8] |= static_cast<uint8_t>(1u << (c % 8));
    }

    // ---- file ---------------------------------------------------------------------------------------------------------------------------------
    stage("writing", 0.96);
    rbsp::Header hdr{};
    std::memcpy(hdr.ident, rbsp::kIdent, 4);
    hdr.version = rbsp::kVersion;
    std::vector<uint8_t>& f = res.bsp;
    f.resize(sizeof(hdr));
    auto lump = [&](rbsp::Lump l, const void* data, size_t bytes) {
        while (f.size() % 4) f.push_back(0);
        hdr.lumps[l].offset = static_cast<int32_t>(f.size());
        hdr.lumps[l].length = static_cast<int32_t>(bytes);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        if (bytes) f.insert(f.end(), p, p + bytes);
    };
    auto vec = [&](rbsp::Lump l, const auto& v) { lump(l, v.data(), v.size() * sizeof(v[0])); };
    ents.push_back('\0');
    lump(rbsp::kEntities, ents.data(), ents.size());
    vec(rbsp::kShaders, shaders.list);
    vec(rbsp::kPlanes, planes.list);
    vec(rbsp::kNodes, nodes);
    vec(rbsp::kLeafs, leafs);
    vec(rbsp::kLeafSurfaces, leafSurfaces);
    vec(rbsp::kLeafBrushes, leafBrushes);
    vec(rbsp::kModels, models);
    vec(rbsp::kBrushes, brushes);
    vec(rbsp::kBrushSides, sides);
    vec(rbsp::kDrawVerts, verts);
    vec(rbsp::kDrawIndexes, indexes);
    lump(rbsp::kFogs, nullptr, 0);
    vec(rbsp::kSurfaces, surfaces);
    lump(rbsp::kLightmaps, nullptr, 0);
    vec(rbsp::kLightGrid, palette);
    vec(rbsp::kVisibility, visLump);
    vec(rbsp::kLightArray, lightArray);
    std::memcpy(f.data(), &hdr, sizeof(hdr));
    stage("done", 1);
    return res;
}

// ---- contents rules ---------------------------------------------------------------------------------------------------------------------------
bool globMatch(const std::string& pattern, const std::string& text) {
    auto low = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || low(pattern[p]) == low(text[t]))) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

namespace {
const char* kDefaultBspConfig = R"BSP({
  "contentFlags": {"solid": 1, "lava": 8, "slime": 16, "water": 32, "fog": 64, "areaportal": 32768, "playerclip": 65536, "monsterclip": 131072,
                   "teleporter": 262144, "jumppad": 524288, "clusterportal": 1048576, "donotenter": 2097152, "origin": 16777216, "detail": 134217728,
                   "structural": 268435456, "translucent": 536870912, "trigger": 1073741824, "nodrop": 2147483648},
  "surfaceFlags": {"nodamage": 1, "slick": 2, "sky": 4, "ladder": 8, "noimpact": 16, "nomarks": 32, "flesh": 64, "nodraw": 128, "hint": 256, "skip": 512,
                   "nolightmap": 1024, "pointlight": 2048, "metalsteps": 4096, "nosteps": 8192, "nonsolid": 16384, "lightfilter": 32768,
                   "alphashadow": 65536, "nodlight": 131072, "dust": 262144, "decal": 1048576},
  "textures": [
    {"match": "common/clip", "contents": ["playerclip"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/playerclip", "contents": ["playerclip"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/monsterclip", "contents": ["monsterclip"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/trigger", "contents": ["trigger"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/caulk", "contents": ["solid"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/nodraw", "contents": ["solid"], "surface": ["nodraw", "nolightmap"]},
    {"match": "common/hint", "contents": [], "surface": ["hint", "nodraw", "nolightmap"]},
    {"match": "common/skip", "contents": [], "surface": ["skip", "nodraw", "nolightmap"]},
    {"match": "common/origin", "contents": ["origin"], "surface": ["nodraw", "nolightmap"]},
    {"match": "*water*", "contents": ["water", "translucent"], "surface": ["nonsolid"]},
    {"match": "*lava*", "contents": ["lava"], "surface": ["nonsolid"]},
    {"match": "*slime*", "contents": ["slime"], "surface": ["nonsolid"]},
    {"match": "*cube", "contents": [], "surface": ["sky", "nolightmap"]},
    {"match": "*sky*", "contents": [], "surface": ["sky", "nolightmap"]}
  ],
  "classes": [
    {"match": "trigger_*", "contents": ["trigger"]}
  ]
})BSP";

uint32_t flagsOf(const nlohmann::json& list, const std::map<std::string, uint32_t>& names, const std::string& where, std::vector<std::string>* warnings) {
    uint32_t f = 0;
    if (!list.is_array()) { if (warnings && !list.is_null()) warnings->push_back(where + ": expected a list of flag names"); return 0; }
    for (const auto& v : list) {
        if (v.is_number_unsigned() || v.is_number_integer()) { f |= static_cast<uint32_t>(v.get<int64_t>()); continue; }
        if (!v.is_string()) continue;
        const auto it = names.find(v.get<std::string>());
        if (it != names.end()) f |= it->second;
        else if (warnings) warnings->push_back(where + ": unknown flag '" + v.get<std::string>() + "'");
    }
    return f;
}

void readRules(const nlohmann::json& list, const BspContentRules& r, std::vector<BspContentRules::Rule>& out, const std::string& where, std::vector<std::string>* warnings) {
    if (!list.is_array()) return;
    for (const auto& e : list) {
        if (!e.is_object() || !e.contains("match") || !e["match"].is_string()) { if (warnings) warnings->push_back(where + ": every rule needs a \"match\" string"); continue; }
        BspContentRules::Rule rule;
        rule.match = e["match"].get<std::string>();
        if (e.contains("contents")) { rule.hasContents = true; rule.contents = flagsOf(e["contents"], r.contentFlags, where + " " + rule.match, warnings); }
        if (e.contains("surface")) rule.surface = flagsOf(e["surface"], r.surfaceFlags, where + " " + rule.match, warnings);
        out.push_back(rule);
    }
}
}  // namespace

BspContentRules BspContentRules::defaults() { return fromJson(nlohmann::json::object()); }

BspContentRules BspContentRules::fromJson(const nlohmann::json& bsp, std::vector<std::string>* warnings) {
    const nlohmann::json def = nlohmann::json::parse(kDefaultBspConfig);
    BspContentRules r;
    for (const auto& [k, v] : def["contentFlags"].items()) r.contentFlags[k] = v.get<uint32_t>();
    for (const auto& [k, v] : def["surfaceFlags"].items()) r.surfaceFlags[k] = v.get<uint32_t>();
    const bool has = bsp.is_object();
    for (const char* table : {"contentFlags", "surfaceFlags"})
        if (has && bsp.contains(table) && bsp[table].is_object())
            for (const auto& [k, v] : bsp[table].items()) {
                if (!v.is_number()) { if (warnings) warnings->push_back(std::string("bsp.") + table + "." + k + " must be a number"); continue; }
                (std::string(table) == "contentFlags" ? r.contentFlags : r.surfaceFlags)[k] = static_cast<uint32_t>(v.get<int64_t>());
            }
    if (has) {
        readRules(bsp.value("textures", nlohmann::json()), r, r.textures, "bsp.textures", warnings);
        readRules(bsp.value("classes", nlohmann::json()), r, r.classes, "bsp.classes", warnings);
    }
    readRules(def["textures"], r, r.textures, "bsp.textures", warnings);
    readRules(def["classes"], r, r.classes, "bsp.classes", warnings);
    return r;
}

BspContentRules::Flags BspContentRules::forTexture(const std::string& texture) const {
    Flags f;
    const auto solid = contentFlags.find("solid");
    f.contents = solid != contentFlags.end() ? solid->second : 1;
    for (const auto& r : textures)
        if (globMatch(r.match, texture)) {
            if (r.hasContents) f.contents = r.contents;
            f.surface = r.surface;
            break;
        }
    return f;
}

bool BspContentRules::forClass(const std::string& classname, uint32_t& contents) const {
    for (const auto& r : classes)
        if (r.hasContents && globMatch(r.match, classname)) { contents = r.contents; return true; }
    return false;
}

bool BspContentRules::isSky(const std::string& texture) const {
    if (isCubemapTexture(texture)) return true;
    const auto it = surfaceFlags.find("sky");
    return it != surfaceFlags.end() && it->second != 0 && (forTexture(texture).surface & it->second) != 0;
}

BspContentRules::Flags BspContentRules::forFace(const std::string& texture) const {
    Flags f = forTexture(texture);
    if (!isSky(texture)) return f;
    f.contents = 0;
    for (const char* name : {"sky", "nolightmap"})
        if (const auto it = surfaceFlags.find(name); it != surfaceFlags.end()) f.surface |= it->second;
    return f;
}

SkySettings skySettingsOf(const Entity& w, const std::function<std::vector<std::string>()>& skyboxes, std::vector<std::string>* warnings) {
    SkySettings s;
    auto key = [&](const char* k) -> std::string { const auto it = w.keyvalues.find(k); return it == w.keyvalues.end() ? std::string() : it->second; };
    const std::string mode = key("_sky_auto");
    if (mode == "box") s.mode = AutoSky::Box;
    else if (mode == "areas") s.mode = AutoSky::Areas;
    else if (!mode.empty() && mode != "off" && warnings) warnings->push_back("worldspawn: _sky_auto must be box or areas; auto sky is off");
    if (const std::string m = key("_sky_margin"); !m.empty()) {
        try { s.margin = std::clamp(std::stod(m), 0.0, 1000.0); } catch (...) { if (warnings) warnings->push_back("worldspawn: _sky_margin is not a number; ignored"); }
    }
    s.texture = key("_sky_texture");
    if (s.texture.empty() && s.mode != AutoSky::Off && skyboxes) {
        const auto list = skyboxes();
        if (!list.empty()) s.texture = list.front();
    }
    return s;
}

std::vector<SkyBox> autoSkyBoxes(const std::vector<LightSurface>& surfaces, AutoSky mode, double margin, const BspContentRules& rules) {
    if (mode == AutoSky::Off) return {};
    // bounds per brush (the pieces of one authored brush belong together)
    auto join = [](SkyBox& a, const SkyBox& b) {
        a.mn = {std::min(a.mn.x, b.mn.x), std::min(a.mn.y, b.mn.y), std::min(a.mn.z, b.mn.z)};
        a.mx = {std::max(a.mx.x, b.mx.x), std::max(a.mx.y, b.mx.y), std::max(a.mx.z, b.mx.z)};
    };
    std::map<std::string, SkyBox> byBrush;
    for (const LightSurface& s : surfaces) {
        if (s.verts.empty() || rules.isSky(s.texture)) continue;
        const std::string key = s.instance + '\x1f' + s.entityId + '\x1f' + s.brushInstance + '\x1f' + s.brushId;
        auto it = byBrush.try_emplace(key, SkyBox{s.verts[0], s.verts[0]}).first;
        for (const Vec3& v : s.verts) join(it->second, SkyBox{v, v});
    }
    std::vector<SkyBox> boxes;
    for (const auto& kv : byBrush) boxes.push_back(kv.second);
    if (boxes.empty()) return {};
    auto grow = [](SkyBox b, double m) { b.mn = b.mn - Vec3{m, m, m}; b.mx = b.mx + Vec3{m, m, m}; return b; };
    if (mode == AutoSky::Box) {
        SkyBox all = boxes[0];
        for (const auto& b : boxes) join(all, b);
        return {grow(all, margin)};
    }
    // merge touching / overlapping boxes until none do: first the brushes themselves (the groups), then the groups' boxes with the margin
    auto touch = [](const SkyBox& a, const SkyBox& b) {
        const double e = 1e-4;
        return a.mn.x <= b.mx.x + e && b.mn.x <= a.mx.x + e && a.mn.y <= b.mx.y + e && b.mn.y <= a.mx.y + e && a.mn.z <= b.mx.z + e && b.mn.z <= a.mx.z + e;
    };
    auto mergeAll = [&](std::vector<SkyBox> list, double m) {
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t i = 0; i < list.size() && !merged; ++i)
                for (size_t j = i + 1; j < list.size(); ++j)
                    if (touch(grow(list[i], m), grow(list[j], m))) { join(list[i], list[j]); list.erase(list.begin() + static_cast<std::ptrdiff_t>(j)); merged = true; break; }
        }
        return list;
    };
    // groups: brushes whose bounds touch, transitively (union-find over the pairs)
    std::vector<size_t> parent(boxes.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j)
            if (touch(boxes[i], boxes[j])) parent[root(i)] = root(j);
    std::map<size_t, SkyBox> byGroup;
    for (size_t i = 0; i < boxes.size(); ++i) {
        auto it = byGroup.find(root(i));
        if (it == byGroup.end()) byGroup.emplace(root(i), boxes[i]); else join(it->second, boxes[i]);
    }
    std::vector<SkyBox> groups;
    for (const auto& kv : byGroup) groups.push_back(kv.second);
    groups = mergeAll(groups, margin);
    for (auto& g : groups) g = grow(g, margin);
    return groups;
}

uint32_t BspContentRules::decalFlag() const {
    const auto it = surfaceFlags.find("decal");
    return it != surfaceFlags.end() ? it->second : 0x100000u;
}

bool BspView::open(const std::vector<uint8_t>& bytes, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (bytes.size() < sizeof(rbsp::Header)) return fail("too small");
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.ident, rbsp::kIdent, 4) != 0 || header.version != rbsp::kVersion) return fail("not an rBSP v2 file");
    for (const auto& l : header.lumps)
        if (l.offset < 0 || l.length < 0 || static_cast<size_t>(l.offset) + static_cast<size_t>(l.length) > bytes.size()) return fail("bad lump");
    data = bytes.data();
    size = bytes.size();
    return true;
}

std::string BspView::entities() const {
    const auto& e = header.lumps[rbsp::kEntities];
    std::string s(reinterpret_cast<const char*>(data + e.offset), static_cast<size_t>(e.length));
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

}  // namespace core
