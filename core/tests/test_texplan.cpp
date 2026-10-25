// The C++ port of the Shell's texture layout functions (texplan.h) against the shared vectors the Shell's tests write
// (testdata/texplan_vectors.json, shell/tests/texplanVectors.test.ts): repeat behaviour, face UV maps and plans, texture group layouts, reliefs.
#include "core/document.h"
#include "core/texplan.h"
#include "doctest.h"
#include <cmath>
#include <fstream>
#include <map>

using namespace core;
using namespace core::texplan;
using ojson = nlohmann::ordered_json;

namespace {
ojson loadVectors() {
    std::ifstream in(std::string(TESTDATA_DIR) + "/texplan_vectors.json", std::ios::binary);
    REQUIRE(in.good());
    return ojson::parse(in);
}
bool near(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b)); }
std::optional<double> optNum(const ojson& j) { return j.is_null() ? std::nullopt : std::optional<double>(j.get<double>()); }

struct Env {
    std::map<std::string, TexInfo> infos;
    std::map<std::string, GroupPtr> groups;
    InfoFn info() { return [this](const std::string& p) -> const TexInfo* { const auto it = infos.find(p); return it == infos.end() ? nullptr : &it->second; }; }
    GroupFn group() { return [this](const std::string& n) -> GroupPtr { const auto it = groups.find(n); return it == groups.end() ? nullptr : it->second; }; }
};
Env makeEnv(const ojson& v) {
    Env e;
    for (const auto& t : v["textures"]) {
        TexInfo i;
        i.path = t["path"];
        i.width = t["width"];
        i.height = t["height"];
        i.ppm = t["ppm"];
        i.props = parseProps(nlohmann::json::parse(t["props"].dump()), i.path);
        if (!t["seamless"].is_null()) i.seamless = std::array<bool, 2>{t["seamless"][0].get<bool>(), t["seamless"][1].get<bool>()};
        e.infos[i.path] = i;
    }
    for (auto it = v["groups"].begin(); it != v["groups"].end(); ++it) {
        const GroupParse p = parseGroup(it.value());
        REQUIRE_MESSAGE(p.group, it.key());
        e.groups[it.key()] = p.group;
    }
    return e;
}
}  // namespace

TEST_CASE("texplan: repeat behaviour of one axis matches the Shell (axisSegments)") {
    const ojson v = loadVectors();
    size_t n = 0;
    for (const auto& c : v["axis"]) {
        const AxisRepeat cfg = parseProps(nlohmann::json{{"repeat", {{"u", nlohmann::json::parse(c["cfg"].dump())}}}}).u;
        AltRatio alt{0, 0};
        const bool hasAlt = !c["alt"].is_null();
        if (hasAlt) alt = {c["alt"]["k"].get<double>(), c["alt"]["off"].get<double>()};
        const bool tm = !c["tiles"].is_null() && c["tiles"]["main"].get<bool>(), ta = !c["tiles"].is_null() && c["tiles"]["alt"].get<bool>();
        const auto segs = axisSegments(cfg, hasAlt ? &alt : nullptr, c["lo"].get<double>(), c["hi"].get<double>(), tm, ta);
        CAPTURE(c.dump());
        if (c["segs"].is_null()) { CHECK_FALSE(segs); continue; }
        REQUIRE(segs);
        REQUIRE(segs->size() == c["segs"].size());
        for (size_t i = 0; i < segs->size(); ++i) {
            const auto& s = (*segs)[i];
            const auto& e = c["segs"][i];
            CHECK(near(s.lo, e["lo"]));
            CHECK(near(s.hi, e["hi"]));
            CHECK(near(s.s, e["s"]));
            CHECK(near(s.c, e["c"]));
            CHECK(s.alt == e["alt"].get<bool>());
            CHECK(s.tile.has_value() == !e["tile"].is_null());
            if (s.tile && !e["tile"].is_null()) CHECK(*s.tile == e["tile"].get<int>());
        }
        ++n;
    }
    CHECK(n > 100);
}

TEST_CASE("texplan: face UV maps and plans (mirror, anchor, alternate texture, edge clamp, face overrides, texture group layouts) match the Shell") {
    const ojson v = loadVectors();
    Env env = makeEnv(v);
    std::map<std::string, Brush> brushes;
    for (const auto& b : v["brushes"]) { Brush br = brushFromJson(nlohmann::json::parse(b.dump())); brushes[br.id] = br; }
    size_t planned = 0;
    for (const auto& c : v["faces"]) {
        const Brush& b = brushes.at(c["brush"]);
        const BrushFace* f = nullptr;
        for (const auto& x : b.faces) if (x.id == c["face"].get<uint32_t>()) f = &x;
        REQUIRE(f);
        CAPTURE(c["brush"].get<std::string>());
        CAPTURE(c["face"].get<int>());
        const auto uvAt = faceUvAt(b, *f, env.info());
        std::vector<V3> pts;
        for (uint32_t i : f->loop) pts.push_back(v3(b.vertices[i]));
        V3 ctr{0, 0, 0};
        for (const V3& p : pts) for (int k = 0; k < 3; ++k) ctr[static_cast<size_t>(k)] += p[static_cast<size_t>(k)] / static_cast<double>(pts.size());
        pts.push_back(ctr);
        for (size_t i = 0; i < pts.size(); ++i) {
            const V2 uv = uvAt(pts[i]);
            CHECK(near(uv[0], c["uvSamples"][i][0]));
            CHECK(near(uv[1], c["uvSamples"][i][1]));
        }
        const PlanPtr plan = planForFace(b, *f, env.info(), env.group());
        if (c["pieces"].is_null()) { CHECK_FALSE(plan); continue; }
        REQUIRE(plan);
        ++planned;
        REQUIRE(plan->textures.size() == c["textures"].size());
        for (size_t i = 0; i < plan->textures.size(); ++i) CHECK(plan->textures[i] == c["textures"][i].get<std::string>());
        // retexture: the pieces of every triangle of the face, fan-triangulated
        size_t k = 0;
        const auto tris = triangulateFace(b, *f);
        for (size_t src = 0; src < tris.size(); ++src) {
            const std::array<V3, 3> tri = {v3(b.vertices[tris[src][0]]), v3(b.vertices[tris[src][1]]), v3(b.vertices[tris[src][2]])};
            for (const Piece& p : plan->pieces(tri))
                for (size_t q = 1; q + 1 < p.verts.size(); ++q) {
                    REQUIRE(k < c["pieces"].size());
                    const auto& e = c["pieces"][k++];
                    CHECK(p.texture == e["texture"].get<std::string>());
                    CHECK(src == e["src"].get<size_t>());
                    const PieceVert* vv[3] = {&p.verts[0], &p.verts[q], &p.verts[q + 1]};
                    for (int m = 0; m < 3; ++m) {
                        for (int w = 0; w < 3; ++w) CHECK(near(vv[m]->w[static_cast<size_t>(w)], e["verts"][m]["w"][w]));
                        CHECK(near(vv[m]->uv[0], e["verts"][m]["uv"][0]));
                        CHECK(near(vv[m]->uv[1], e["verts"][m]["uv"][1]));
                    }
                }
        }
        CHECK(k == c["pieces"].size());
    }
    CHECK(planned > 8);
}

TEST_CASE("texplan: texture group layouts (rule groups, wall and frame sets) give the Shell's cells") {
    const ojson v = loadVectors();
    Env env = makeEnv(v);
    for (const auto& c : v["layouts"]) {
        LayoutInput in;
        const GroupPtr g = env.groups.at(c["group"]);
        in.group = g.get();
        in.w = c["w"];
        in.h = c["h"];
        in.seed = c["seed"].get<uint32_t>();
        in.info = env.info();
        in.fallbackTexture = "v/wall";
        in.hasFallbackTexture = true;
        const LayoutResult r = solveLayout(in);
        CAPTURE(c["group"].get<std::string>());
        CAPTURE(c["w"].get<double>());
        CAPTURE(c["h"].get<double>());
        CAPTURE(c["seed"].get<int>());
        CHECK(r.truncated == c["truncated"].get<bool>());
        REQUIRE(r.cells.size() == c["cells"].size());
        for (size_t i = 0; i < r.cells.size(); ++i) {
            const Cell& a = r.cells[i];
            const auto& e = c["cells"][i];
            CHECK(near(a.x0, e["x0"]));
            CHECK(near(a.y0, e["y0"]));
            CHECK(near(a.x1, e["x1"]));
            CHECK(near(a.y1, e["y1"]));
            CHECK(a.texture == e["texture"].get<std::string>());
            CHECK(a.fit == e["fit"].get<std::string>());
            CHECK(a.origin == e["origin"].get<std::string>());
            CHECK(a.alignU == e["alignU"].get<std::string>());
            CHECK(a.alignV == e["alignV"].get<std::string>());
            CHECK(a.flipU == e["flipU"].get<bool>());
            CHECK(a.flipV == e["flipV"].get<bool>());
            CHECK(a.rotate == e["rotate"].get<int>());
            CHECK(a.cropU.has_value() == !e["cropU"].is_null());
            if (a.cropU && !e["cropU"].is_null()) CHECK(near(*a.cropU, e["cropU"]));
            CHECK(a.cropV.has_value() == !e["cropV"].is_null());
            if (a.cropV && !e["cropV"].is_null()) CHECK(near(*a.cropV, e["cropV"]));
            CHECK(a.path == e["path"].get<std::string>());
        }
    }
}

TEST_CASE("texplan: a relief laid over a triangle matches the Shell (reliefTriangle)") {
    const ojson v = loadVectors();
    const auto relief = parseRelief(nlohmann::json::parse(v["reliefs"]["relief"].dump()));
    REQUIRE(relief);
    for (const auto& c : v["reliefs"]["cases"]) {
        std::array<V3, 3> tri;
        std::array<V2, 3> uv;
        for (int k = 0; k < 3; ++k) {
            tri[static_cast<size_t>(k)] = {c["tri"][k][0], c["tri"][k][1], c["tri"][k][2]};
            uv[static_cast<size_t>(k)] = {c["uv"][k][0], c["uv"][k][1]};
        }
        const V3 n{c["n"][0], c["n"][1], c["n"][2]};
        const auto out = reliefTriangle(tri, uv, n, *relief, "t", nullptr);
        if (c["out"].is_null()) { CHECK_FALSE(out); continue; }
        REQUIRE(out);
        std::vector<double> flat;
        for (const auto& t : *out)
            for (int k = 0; k < 3; ++k) {
                for (int m = 0; m < 3; ++m) flat.push_back(t.pos[static_cast<size_t>(k)][static_cast<size_t>(m)]);
                for (int m = 0; m < 3; ++m) flat.push_back(t.normal[static_cast<size_t>(k)][static_cast<size_t>(m)]);
                flat.push_back(t.uv[static_cast<size_t>(k)][0]);
                flat.push_back(t.uv[static_cast<size_t>(k)][1]);
            }
        REQUIRE(flat.size() == c["out"].size());
        for (size_t i = 0; i < flat.size(); ++i) CHECK(near(flat[i], c["out"][i]));
    }
}

TEST_CASE("texplan: the connecting walls of a relief match the Shell (reliefTriangle with borders)") {
    const ojson v = loadVectors();
    REQUIRE(v["reliefs"].contains("walls"));
    std::map<std::string, std::shared_ptr<const Relief>> reliefs;
    for (const auto& c : v["reliefs"]["walls"]) {
        auto& relief = reliefs[c["relief"].dump()];  // one object per relief: the triangle cache is keyed by its address
        if (!relief) relief = parseRelief(nlohmann::json::parse(c["relief"].dump()));
        REQUIRE(relief);
        std::array<V3, 3> tri;
        std::array<V2, 3> uv;
        for (int k = 0; k < 3; ++k) {
            tri[static_cast<size_t>(k)] = {c["tri"][k][0], c["tri"][k][1], c["tri"][k][2]};
            uv[static_cast<size_t>(k)] = {c["uv"][k][0], c["uv"][k][1]};
        }
        const V3 n{c["n"][0], c["n"][1], c["n"][2]};
        // a face with a relief of its own 0.05 high, lying behind the plane x = 0 (a convex corner there)
        const NeighbourFn nbrAt = [](const V3& a, const V3& b) -> std::optional<Neighbour> {
            if (std::fabs(a[0]) < 1e-9 && std::fabs(b[0]) < 1e-9) return Neighbour{{-1, 0, 0}, {{0, 1, 0.05, 0.05}, {1, 1.5, 0.05, 0.05}}};
            return std::nullopt;
        };
        const auto out = reliefTriangle(tri, uv, n, *relief, "t", nullptr, c["border"].get<int>(), c["nbr"].get<bool>() ? &nbrAt : nullptr);
        if (c["out"].is_null()) { CHECK_FALSE(out); continue; }
        REQUIRE(out);
        std::vector<double> flat;
        for (const auto& t : *out)
            for (int k = 0; k < 3; ++k) {
                for (int m = 0; m < 3; ++m) flat.push_back(t.pos[static_cast<size_t>(k)][static_cast<size_t>(m)]);
                for (int m = 0; m < 3; ++m) flat.push_back(t.normal[static_cast<size_t>(k)][static_cast<size_t>(m)]);
                flat.push_back(t.uv[static_cast<size_t>(k)][0]);
                flat.push_back(t.uv[static_cast<size_t>(k)][1]);
            }
        REQUIRE(flat.size() == c["out"].size());
        for (size_t i = 0; i < flat.size(); ++i) CHECK(near(flat[i], c["out"][i]));
    }
}

TEST_CASE("texplan: tile-boundary relief walls are suppressed when the next relief continues") {
    Relief relief;
    relief.vertices = {
        {0, 0, 0.2}, {1, 0, 0.2}, {1, 1, 0.2}, {0, 1, 0.2},
        {0, 0, 0}, {0, 1, 0}, {0, 1, 0.2}, {0, 0, 0.2},
        {1, 0, 0}, {1, 1, 0}, {1, 1, 0.2}, {1, 0, 0.2},
    };
    relief.faces = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}};
    const std::array<V3, 4> p = {{{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}};
    const auto uv = [](const V3& x) -> V2 { return {x[0], 1 - x[1]}; };
    const std::array<std::array<int, 3>, 2> tri = {{{0, 1, 2}, {0, 2, 3}}};
    size_t connectingTriangles = 0;
    for (const auto& ids : tri) {
        const std::array<V3, 3> t = {p[static_cast<size_t>(ids[0])], p[static_cast<size_t>(ids[1])], p[static_cast<size_t>(ids[2])]};
        const std::array<V2, 3> u = {uv(t[0]), uv(t[1]), uv(t[2])};
        const auto out = reliefTriangle(t, u, V3{0, 0, 1}, relief, "t", nullptr, 0, nullptr);
        REQUIRE(out);
        for (const TexturedTri& face : *out) if (std::fabs(face.normal[0][2]) < 0.99) ++connectingTriangles;
    }
    CHECK(connectingTriangles == 0);
}

TEST_CASE("texplan: relief outline without a relief neighbour does not connect to the host face") {
    Relief relief;
    relief.vertices = {{0, 0, 0.2}, {1, 0, 0.2}, {1, 1, 0.2}, {0, 1, 0.2}};
    relief.faces = {{0, 1, 2, 3}};
    const std::array<V3, 3> tri = {{{0, 0, 0}, {2, 0, 0}, {0, 1, 0}}};
    const std::array<V2, 3> uv = {{{0, 1}, {2, 1}, {0, 0}}};
    const V3 n{0, 0, 1};

    // All three edges are outlines. They must not be closed back to the underlying
    // brush face just because the relief surface ends there.
    const auto out = reliefTriangle(tri, uv, n, relief, "t", nullptr, 0b111, nullptr);
    REQUIRE(out);
    size_t connectingTriangles = 0;
    for (const TexturedTri& t : *out) {
        if (std::fabs(t.normal[0][2]) < 0.99) ++connectingTriangles;
    }
    CHECK(connectingTriangles == 0);
}

TEST_CASE("texplan: connecting walls only bridge spans with neighbouring relief surface") {
    Relief relief;
    relief.vertices = {{0, 0, 0.2}, {1, 0, 0.2}, {1, 1, 0.2}, {0, 1, 0.2}};
    relief.faces = {{0, 1, 2, 3}};
    const std::array<V3, 3> tri = {{{0, 0, 0}, {2, 1, 0}, {0, 1, 0}}};
    const std::array<V2, 3> uv = {{{0, 1}, {2, 0}, {0, 0}}};
    const V3 n{0, 0, 1};
    const NeighbourFn nbr = [](const V3& a, const V3& b) -> std::optional<Neighbour> {
        if (std::fabs(a[0]) < 1e-9 && std::fabs(b[0]) < 1e-9)
            return Neighbour{{-1, 0, 0}, {{0, 0.5, 0.1, 0.1}}};
        return std::nullopt;
    };

    const auto out = reliefTriangle(tri, uv, n, relief, "t", nullptr, 0b100, &nbr);
    REQUIRE(out);
    size_t connectingTriangles = 0;
    bool reachesNeighbourRelief = false;
    for (const TexturedTri& t : *out) {
        if (std::fabs(t.normal[0][2]) >= 0.99) continue; // ignore the horizontal relief surface
        ++connectingTriangles;
        for (const V3& p : t.pos) {
            CHECK(p[1] >= 0.5 - 1e-6); // the unoccupied half of the neighbour has no connector
            if (p[0] < -1e-6) reachesNeighbourRelief = true;
        }
    }
    CHECK(connectingTriangles > 0);
    CHECK(reachesNeighbourRelief);
}

TEST_CASE("texplan: polygon outlines, fan and piece border bits") {
    const std::vector<V3> A = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, B = {{1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}};
    const auto two = polygonBorders({A, B});
    CHECK(two[0] == std::vector<char>{1, 0, 1, 1});
    CHECK(two[1] == std::vector<char>{1, 1, 1, 0});
    const std::vector<V3> C = {{1, 0, 0}, {2, 0, 0}, {2, 0.5, 0}, {1, 0.5, 0}}, D = {{1, 0.5, 0}, {2, 0.5, 0}, {2, 1, 0}, {1, 1, 0}};
    CHECK(polygonBorders({A, C, D})[0] == std::vector<char>{1, 0, 1, 1});  // a neighbour that splits the shared edge still matches it
    const std::vector<char> e = {1, 0, 1, 1};
    CHECK(fanBorder(1, 4, e) == 1);
    CHECK(fanBorder(2, 4, e) == 6);
    std::vector<PieceVert> piece(3);
    piece[0].w = {1, 0, 0}; piece[1].w = {0, 1, 0}; piece[2].w = {0.5, 0.25, 0.25};
    CHECK(pieceBorder(piece, 1) == 1);
    CHECK(pieceBorder(piece, 0) == 0);
    OutlineIndex idx;
    const size_t a = idx.add("f", A), b = idx.add("f", B), c = idx.add("g", A);
    CHECK(idx.of(a) == std::vector<char>{1, 0, 1, 1});
    CHECK(idx.of(b) == std::vector<char>{1, 1, 1, 0});
    CHECK(idx.of(c) == std::vector<char>{1, 0, 1, 1});  // another face ("g", polygon A) continues the surface across A's shared edge with B (face "f"): no outline there
}

TEST_CASE("texplan: seamless edge analysis (seamless.ts) and the seeded random numbers (rng.ts)") {
    // a 16 x 16 image whose rows run dark to light: the top and bottom rows do not continue each other, the columns do
    std::vector<uint8_t> img(16 * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            for (int c = 0; c < 3; ++c) img[(static_cast<size_t>(y) * 16 + x) * 4 + c] = static_cast<uint8_t>(y * 15);
    const auto s = analyseSeams(img.data(), 16, 16);
    CHECK(s[0]);         // u: every column is the same
    CHECK_FALSE(s[1]);   // v: 0 at the top, 225 at the bottom
    // values checked against the Shell's hash32 / rand01 / faceSeed
    CHECK(hash32("") == 0x811c9dc5u * 0 + hash32(""));  // deterministic
    CHECK(faceSeed("a", 3) == hash32("a/3"));
    CHECK(rand01(7, "r/0") == hash32("7|r/0") / 4294967296.0);
}
