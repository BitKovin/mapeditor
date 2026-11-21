// T11.3b: Flow, hotspots, the texel density heat map and the Fast Texture Tool model (ports of shell/tests/flow.test.ts, hotspots.test.ts, texelDensity.test.ts and
// ftt.test.ts). Pure, no window, no GPU.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "tools/texturing/density.h"
#include "tools/texturing/flow.h"
#include "tools/texturing/ftt.h"
#include "tools/texturing/hotspots.h"

using namespace native::tools::tex;
namespace tp = core::texplan;

namespace {

InfoFn texture(int w, int h, double ppm = 64) {
    static std::map<std::tuple<int, int, double>, tp::TexInfo> infos;
    const auto key = std::make_tuple(w, h, ppm);
    auto it = infos.find(key);
    if (it == infos.end()) {
        tp::TexInfo i;
        i.path = "t";
        i.width = w;
        i.height = h;
        i.ppm = ppm;
        i.props = tp::defaultProps();
        it = infos.emplace(key, i).first;
    }
    const tp::TexInfo* p = &it->second;
    return [p](const std::string&) { return p; };
}
const InfoFn kNone{};

const core::BrushFace* faceWith(const core::Brush& b, int axis, double sign) {
    for (const core::BrushFace& f : b.faces)
        if (tp::faceNormalOf(b, f)[static_cast<size_t>(axis)] * sign > 0.99) return &f;
    return nullptr;
}

void close(const V2& a, const V2& b, double eps = 1e-6) {
    CHECK(a[0] == doctest::Approx(b[0]).epsilon(eps));
    CHECK(a[1] == doctest::Approx(b[1]).epsilon(eps));
}

double lo(const std::vector<V2>& uv, int k) {
    double m = 1e300;
    for (const V2& q : uv) m = std::min(m, q[static_cast<size_t>(k)]);
    return m;
}
double hi(const std::vector<V2>& uv, int k) {
    double m = -1e300;
    for (const V2& q : uv) m = std::max(m, q[static_cast<size_t>(k)]);
    return m;
}

V2 uvAt(const core::Brush& b, const core::BrushFace& f, const V3& q, const InfoFn& info) {
    const tp::UvAffine m = tp::faceAffine(b, f, info);
    return {tp::evalAff(m.u, q), tp::evalAff(m.v, q)};
}

// box x 0..4, y 0..2, z 0..1 with the +z wall textured "road" with an explicit projection
struct FlowBox {
    core::Brush b;
    uint32_t front = 0;
    InfoFn info = kNone;
    FlowBox() {
        b = core::Brush::makeBox("b", {0, 0, 0}, {4, 2, 1}, "t");
        front = faceWith(b, 2, 1)->id;
        Projection p;
        p.scale = {2, 2};
        p.shift = {0.25, 0.5};
        for (core::BrushFace& f : b.faces)
            if (f.id == front) {
                f.texture = "road";
                f = withProjection(b, f, p);
            }
    }
    const core::BrushFace& face(uint32_t id) const {
        for (const core::BrushFace& f : b.faces)
            if (f.id == id) return f;
        throw std::runtime_error("no face");
    }
};

} // namespace

// ---- flow --------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("flow: continues around a corner, the neighbouring wall is unfolded into the front wall plane, nothing stretched") {
    FlowBox fb;
    const uint32_t right = faceWith(fb.b, 0, 1)->id;
    const auto r = flowFaces(fb.b, fb.front, {right}, fb.info);
    REQUIRE(r);
    CHECK(r->flowed == std::vector<uint32_t>{right});
    const core::BrushFace* nr = nullptr;
    for (const core::BrushFace& f : r->brush.faces)
        if (f.id == right) nr = &f;
    REQUIRE(nr);
    CHECK(nr->texture == "road");
    const core::BrushFace* fr = nullptr;
    for (const core::BrushFace& f : r->brush.faces)
        if (f.id == fb.front) fr = &f;
    for (double y : {0.0, 1.0, 2.0}) {   // the shared edge (x = 4, z = 1) has the same UV on both faces
        close(uvAt(r->brush, *fr, {4, y, 1}, kNone), uvAt(r->brush, *nr, {4, y, 1}, kNone), 1e-9);
    }
    // going back along the right wall (z from 1 to 0) continues u: u = (4 + (1 - z)) / 2 + 0.25
    CHECK(uvAt(r->brush, *nr, {4, 1, 0}, kNone)[0] == doctest::Approx((4 + 1) / 2.0 + 0.25).epsilon(1e-9));
    CHECK(uvAt(r->brush, *nr, {4, 1, 0}, kNone)[1] == doctest::Approx(uvAt(r->brush, *fr, {4, 1, 1}, kNone)[1]).epsilon(1e-9));
    CHECK(uvAt(r->brush, *nr, {4, 1, 0}, kNone)[0] - uvAt(r->brush, *nr, {4, 1, 1}, kNone)[0] == doctest::Approx(0.5).epsilon(1e-9));   // 1 m of wall = 0.5 repeat
}

TEST_CASE("flow: over the top edge v continues upwards") {
    FlowBox fb;
    const uint32_t top = faceWith(fb.b, 1, 1)->id;
    const auto r = flowFaces(fb.b, fb.front, {top}, fb.info);
    REQUIRE(r);
    const core::BrushFace *nt = nullptr, *fr = nullptr;
    for (const core::BrushFace& f : r->brush.faces) {
        if (f.id == top) nt = &f;
        if (f.id == fb.front) fr = &f;
    }
    CHECK(uvAt(r->brush, *nt, {1, 2, 0}, kNone)[1] == doctest::Approx(uvAt(r->brush, *fr, {1, 2, 1}, kNone)[1] - 0.5).epsilon(1e-9));
    CHECK(uvAt(r->brush, *nt, {1, 2, 1}, kNone)[0] == doctest::Approx(uvAt(r->brush, *fr, {1, 2, 1}, kNone)[0]).epsilon(1e-9));
}

TEST_CASE("flow: a target two faces away flows through the face in between (shortest chain), in order") {
    FlowBox fb;
    const uint32_t back = faceWith(fb.b, 2, -1)->id;
    const auto r = flowFaces(fb.b, fb.front, {back}, fb.info);
    REQUIRE(r);
    REQUIRE(r->flowed.size() == 2);
    CHECK(r->flowed.back() == back);
    const core::BrushFace *mid = nullptr, *nb = nullptr;
    for (const core::BrushFace& f : r->brush.faces) {
        if (f.id == r->flowed[0]) mid = &f;
        if (f.id == back) nb = &f;
    }
    std::vector<uint32_t> shared;
    for (uint32_t v : mid->loop)
        if (std::find(nb->loop.begin(), nb->loop.end(), v) != nb->loop.end()) shared.push_back(v);
    REQUIRE(shared.size() == 2);
    for (uint32_t v : shared) {   // the faces agree along their shared edge
        const size_t a = std::find(mid->loop.begin(), mid->loop.end(), v) - mid->loop.begin(), c = std::find(nb->loop.begin(), nb->loop.end(), v) - nb->loop.begin();
        CHECK(mid->uvs[a].u == doctest::Approx(nb->uvs[c].u).epsilon(1e-9));
        CHECK(mid->uvs[a].v == doctest::Approx(nb->uvs[c].v).epsilon(1e-9));
    }
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;   // nothing stretched
    for (const core::Vec2& q : nb->uvs) {
        u0 = std::min(u0, q.u);
        u1 = std::max(u1, q.u);
        v0 = std::min(v0, q.v);
        v1 = std::max(v1, q.v);
    }
    CHECK(std::hypot(u1 - u0, v1 - v0) * 2 == doctest::Approx(std::hypot(4, 2)).epsilon(1e-9));
}

TEST_CASE("flow: flowed faces get an editable projection that reproduces their UVs exactly") {
    FlowBox fb;
    const uint32_t right = faceWith(fb.b, 0, 1)->id;
    const auto r = flowFaces(fb.b, fb.front, {right}, fb.info);
    REQUIRE(r);
    const core::BrushFace* nr = nullptr;
    for (const core::BrushFace& f : r->brush.faces)
        if (f.id == right) nr = &f;
    const Projection p = projectionOfFace(*nr, kNone);
    CHECK(p.kind == tp::ProjKind::Free);
    const auto re = projectFace(r->brush, *nr, p);
    REQUIRE(re.size() == nr->uvs.size());
    for (size_t i = 0; i < re.size(); ++i) {
        CHECK(re[i][0] == doctest::Approx(nr->uvs[i].u).epsilon(1e-8));
        CHECK(re[i][1] == doctest::Approx(nr->uvs[i].v).epsilon(1e-8));
    }
    CHECK(nr->lookHash != 0);
}

TEST_CASE("flow: the source is unchanged; unreachable / unknown ids give nothing; an empty target list gives nothing") {
    FlowBox fb;
    const uint32_t right = faceWith(fb.b, 0, 1)->id;
    const auto r = flowFaces(fb.b, fb.front, {right}, fb.info);
    REQUIRE(r);
    for (const core::BrushFace& f : r->brush.faces)
        if (f.id == fb.front) {
            CHECK(f.uvs.size() == fb.face(fb.front).uvs.size());
            CHECK(f.lookHash == fb.face(fb.front).lookHash);
        }
    CHECK_FALSE(flowFaces(fb.b, 999, {right}, fb.info));
    CHECK_FALSE(flowFaces(fb.b, fb.front, {999}, fb.info));
    CHECK_FALSE(flowFaces(fb.b, fb.front, {}, fb.info));
}

TEST_CASE("flow: the source repeat override travels with the texture") {
    FlowBox fb;
    const uint32_t right = faceWith(fb.b, 0, 1)->id;
    for (core::BrushFace& f : fb.b.faces)
        if (f.id == fb.front) {
            nlohmann::json look = *f.look;
            look["repeat"] = {{"u", {{"mode", "mirror"}}}};
            f.look = std::make_shared<const nlohmann::json>(look);
        }
    const auto r = flowFaces(fb.b, fb.front, {right}, fb.info);
    REQUIRE(r);
    for (const core::BrushFace& f : r->brush.faces)
        if (f.id == right) {
            const nlohmann::json* rep = tp::faceKey(f, "repeat");
            REQUIRE(rep);
            CHECK((*rep)["u"]["mode"] == "mirror");
        }
}

// ---- hotspots ----------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("hotspots: parse keeps valid rectangles clamped into the texture, drops junk, caps the count; round trip") {
    const auto spots = parseHotspots(nlohmann::json::parse(R"([{"x":-1,"y":0.5,"w":5,"h":0.2},{"x":"a"},null,{"x":0.2,"y":0.2,"w":0.001,"h":0.3}])"));
    REQUIRE(spots.size() == 2);
    CHECK(spots[0] == Hotspot{0, 0.5, 1, 0.2});
    CHECK(spots[1] == Hotspot{0.2, 0.2, 0.01, 0.3});
    CHECK(parseHotspots(nlohmann::json("x")).empty());
    nlohmann::json many = nlohmann::json::array();
    for (int i = 0; i < 100; ++i) many.push_back({{"x", 0}, {"y", 0}, {"w", 0.5}, {"h", 0.5}});
    CHECK(parseHotspots(many).size() == 64);
    const std::vector<Hotspot> rt = {{0.1, 0.2, 0.3, 0.4}};
    CHECK(parseHotspots(hotspotsJson(rt)) == rt);
}

TEST_CASE("hotspots: face size follows the face axes; the choice is the least distorted, then the closest size") {
    const core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {3, 2, 0.2}, "t");
    const V2 fs = faceSize(b, *faceWith(b, 2, 1));
    CHECK(fs[0] == doctest::Approx(3));
    CHECK(fs[1] == doctest::Approx(2));
    const TexSize info{512, 512, 64};   // 8 m per repeat
    const std::vector<Hotspot> spots = {{0, 0, 1, 0.25}, {0, 0.25, 0.25, 0.75}, {0.25, 0.25, 0.5, 0.5}};
    CHECK(chooseHotspot(spots, info, {8, 2}) == 0);   // a wide strip
    CHECK(chooseHotspot(spots, info, {2, 6}) == 1);   // a tall strip
    CHECK(chooseHotspot(spots, info, {4, 4}) == 2);   // a square
    const std::vector<Hotspot> two = {{0, 0, 0.25, 0.25}, {0.5, 0.5, 0.5, 0.5}};
    CHECK(chooseHotspot(two, info, {1.9, 1.9}) == 0);
    CHECK(chooseHotspot(two, info, {3.9, 3.9}) == 1);
    CHECK(chooseHotspot({}, info, {1, 1}) == -1);
    CHECK(hotspotSize(two[1], info) == V2{4, 4});
}

TEST_CASE("hotspots: the projection shows exactly the hotspot (inset a little), upright") {
    const core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {3, 2, 0.2}, "t");
    const core::BrushFace& f = *faceWith(b, 2, 1);
    const Hotspot h{0.25, 0.5, 0.25, 0.25};
    const TexSize info{512, 512, 64};
    const auto uv = projectFace(b, f, hotspotProjection(b, f, h, info));
    const double i = tp::edgeInsetTexels(512) / 512;
    CHECK(lo(uv, 0) == doctest::Approx(h.x + i).epsilon(1e-9));
    CHECK(hi(uv, 0) == doctest::Approx(h.x + h.w - i).epsilon(1e-9));
    CHECK(lo(uv, 1) == doctest::Approx(h.y + i).epsilon(1e-9));
    CHECK(hi(uv, 1) == doctest::Approx(h.y + h.h - i).epsilon(1e-9));
    for (size_t k = 0; k < f.loop.size(); ++k)   // v grows down the wall, like the texture
        if (b.vertices[f.loop[k]].y == 2) CHECK(uv[k][1] == doctest::Approx(h.y + i).epsilon(1e-9));
}

TEST_CASE("hotspots: rectangle helpers") {
    const Hotspot r = rectFromCorners(0.8, 0.9, 0.2, 0.3);
    CHECK(r.x == doctest::Approx(0.2));
    CHECK(r.y == doctest::Approx(0.3));
    CHECK(r.w == doctest::Approx(0.6));
    CHECK(r.h == doctest::Approx(0.6));
    const Hotspot tiny = rectFromCorners(0.5, 0.5, 0.5, 0.5);
    CHECK(tiny.w == doctest::Approx(0.01));
    const Hotspot edge = rectFromCorners(0.99, 0.99, 1.2, 1.2);
    CHECK(edge.x + edge.w <= 1 + 1e-12);
    const Hotspot m = moveRect({0.5, 0.5, 0.4, 0.4}, 1, -1);
    CHECK(m.x == doctest::Approx(0.6));
    CHECK(m.y == doctest::Approx(0));
    const std::vector<Hotspot> hs = {{0, 0, 1, 1}, {0.4, 0.4, 0.2, 0.2}};
    CHECK(hotspotAt(hs, 0.5, 0.5) == 1);
    CHECK(hotspotAt(hs, 0.1, 0.1) == 0);
    CHECK(hotspotAt({}, 0.1, 0.1) == -1);
}

// ---- texel density -----------------------------------------------------------------------------------------------------------------------------------

namespace {
struct Wall {
    core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {4, 4, 0.2}, "t");
    core::BrushFace f;
    explicit Wall(V2 scale) {
        f = *faceWith(b, 2, 1);
        Projection p;
        p.scale = scale;
        f = withProjection(b, f, p);
    }
};
} // namespace

TEST_CASE("texel density: pixels per metre of surface; 0 octaves as authored; stretched negative, squeezed positive, the worse axis wins") {
    const InfoFn info = texture(256, 256, 64);
    const tp::TexInfo* ti = info("t");
    {
        Wall w({4, 4});
        const V2 d = faceDensity(w.b, w.f, 256, 256, kNone);
        CHECK(d[0] == doctest::Approx(64).epsilon(1e-6));
        CHECK(d[1] == doctest::Approx(64).epsilon(1e-6));
        CHECK(*faceHeat(w.b, w.f, ti, kNone) == doctest::Approx(0).epsilon(1e-6));
    }
    { Wall w({8, 8}); CHECK(*faceHeat(w.b, w.f, ti, kNone) == doctest::Approx(-1).epsilon(1e-6)); }
    { Wall w({2, 2}); CHECK(*faceHeat(w.b, w.f, ti, kNone) == doctest::Approx(1).epsilon(1e-6)); }
    { Wall w({4, 16}); CHECK(*faceHeat(w.b, w.f, ti, kNone) == doctest::Approx(-2).epsilon(1e-6)); }   // v is 4x stretched
    {   // doubling the authored ppm makes the same face look stretched
        Wall w({4, 4});
        const tp::TexInfo* t128 = texture(256, 256, 128)("t");
        CHECK(*faceHeat(w.b, w.f, t128, kNone) == doctest::Approx(-1).epsilon(1e-6));
        CHECK_FALSE(faceHeat(w.b, w.f, nullptr, kNone));   // not loaded
    }
}

TEST_CASE("texel density: colours, buckets and labels") {
    const auto g = heatColor(0), r = heatColor(-kHeatRange), bl = heatColor(kHeatRange);
    CHECK(g[1] > g[0]);
    CHECK(g[1] > g[2]);
    CHECK(r[0] > r[1]);
    CHECK(bl[2] > bl[0]);
    CHECK(heatBucket(0) == (kHeatBuckets - 1) / 2);
    CHECK(heatBucket(-99) == 0);
    CHECK(heatBucket(99) == kHeatBuckets - 1);
    CHECK(bucketColor(0) == heatColor(-kHeatRange));
    CHECK(heatLabel(0) == "as authored");
    CHECK(heatLabel(-1).find("stretched 2.0x") != std::string::npos);
    CHECK(heatLabel(1).find("squeezed 2.0x") != std::string::npos);
}

TEST_CASE("natural size default: a face without a projection shows the texture at pixels / ppm, an explicit scale is kept, unloaded = 2 m") {
    Wall w({4, 4});
    core::BrushFace bare = *faceWith(w.b, 2, 1);
    CHECK(projectionOfFace(bare, kNone).scale == V2{2, 2});
    CHECK(projectionOfFace(bare, texture(256, 256, 64)).scale == V2{4, 4});
    CHECK(defaultProjectionFor("t", texture(256, 256, 128)).scale == V2{2, 2});
    CHECK(projectionOfFace(w.f, texture(256, 256, 128)).scale == V2{4, 4});   // explicit, old maps do not change
}

// ---- Fast Texture Tool -------------------------------------------------------------------------------------------------------------------------------

namespace {
core::Brush fttBox() { return core::Brush::makeBox("b", {0, 0, 0}, {4, 2, 1}, "dev/wall"); }
const InfoFn kTex = texture(64, 64, 64);
} // namespace

TEST_CASE("ftt: transform algebra") {
    close(applyXform(translateXform(1, 2), {3, 4}), {4, 6});
    close(applyXform(rotateXformAbout(90, {0, 0}), {1, 0}), {0, 1});   // clockwise on screen: u right becomes v down
    close(applyXform(rotateXformAbout(90, {1, 1}), {2, 1}), {1, 2});
    close(applyXform(scaleXformAbout(2, 3, {1, 1}), {2, 2}), {3, 4});
    close(applyXform(scaleXformAbout(2, 3, {1, 1}), {1, 1}), {1, 1});
    close(applyXform(mirrorXformAbout('u', 2), {3, 5}), {1, 5});
    close(applyXform(mirrorXformAbout('v', 2), {3, 5}), {3, -1});
    const Xform t = composeXform(translateXform(1, 0), scaleXformAbout(2, 2, {0, 0}));   // scale first, then move
    close(applyXform(t, {1, 1}), {3, 2});
    close(applyXform(composeXform(kIdentity, t), {1, 1}), {3, 2});
}

TEST_CASE("ftt: transformFaces moves every corner exactly where the transform says; other faces stay; a collapse is refused; the projection stays editable") {
    const core::Brush b = fttBox();
    const uint32_t front = faceWith(b, 2, 1)->id;
    const std::vector<Xform> ts = {translateXform(0.5, -0.25), rotateXformAbout(33, {1, 0.5}), scaleXformAbout(0.5, 2, {0, 0}), mirrorXformAbout('u', 1.5), mirrorXformAbout('v', 0.4),
                                   composeXform(rotateXformAbout(17, {0, 0}), scaleXformAbout(2, 1, {1, 1}))};
    for (const Xform& t : ts) {
        const auto before = islandPolygons(b, {front}, kTex);
        const FttResult r = transformFaces(b, {front}, t, kTex);
        CHECK(r.changed == 1);
        for (const IslandFace& isl : before) {
            const core::BrushFace* nf = nullptr;
            for (const core::BrushFace& f : r.brush.faces)
                if (f.id == isl.id) nf = &f;
            const auto after = faceUvPolygon(r.brush, *nf, kTex);
            for (size_t i = 0; i < isl.poly.size(); ++i) close(after[i], applyXform(t, isl.poly[i]));
        }
    }
    const FttResult r = transformFaces(b, {front}, translateXform(1, 1), kTex);
    for (size_t i = 0; i < b.faces.size(); ++i)
        if (b.faces[i].id != front) CHECK(r.brush.faces[i].uvs.size() == b.faces[i].uvs.size());
    CHECK(transformFaces(b, {999}, translateXform(1, 1), kTex).changed == 0);
    CHECK(transformFaces(b, {front}, scaleXformAbout(0, 0, {0, 0}), kTex).changed == 0);
    const core::BrushFace* nf = nullptr;
    for (const core::BrushFace& f : r.brush.faces)
        if (f.id == front) nf = &f;
    CHECK(projectionOfFace(*nf, kTex).kind == tp::ProjKind::Free);
    CHECK(nf->uvs.size() == nf->loop.size());
}

TEST_CASE("ftt: an island of two faces moves as one, a texture that continues over their shared edge still does") {
    const core::Brush b0 = fttBox();
    const uint32_t a = faceWith(b0, 2, 1)->id, c = faceWith(b0, 0, 1)->id;
    const FttResult flowed = unwrapFaces(b0, {a, c}, UnwrapMode::Follow, kTex);
    CHECK(flowed.changed > 0);
    const auto seam = [&](const core::Brush& b) {
        std::vector<std::pair<V2, V2>> out;
        const core::BrushFace *A = nullptr, *C = nullptr;
        for (const core::BrushFace& f : b.faces) {
            if (f.id == a) A = &f;
            if (f.id == c) C = &f;
        }
        const auto pa = faceUvPolygon(b, *A, kTex), pc = faceUvPolygon(b, *C, kTex);
        for (size_t i = 0; i < A->loop.size(); ++i) {
            const auto it = std::find(C->loop.begin(), C->loop.end(), A->loop[i]);
            if (it != C->loop.end()) out.push_back({pa[i], pc[static_cast<size_t>(it - C->loop.begin())]});
        }
        return out;
    };
    for (const auto& [x, y] : seam(flowed.brush)) close(x, y, 1e-5);
    const FttResult moved = transformFaces(flowed.brush, {a, c}, composeXform(rotateXformAbout(25, {0.3, 0.2}), scaleXformAbout(1.5, 1.5, {0, 0})), kTex);
    CHECK(moved.changed == 2);
    const auto s = seam(moved.brush);
    CHECK(s.size() == 2);
    for (const auto& [x, y] : s) close(x, y, 1e-5);
}

TEST_CASE("ftt: Mirror U / V flips the island about its centre; twice is the identity") {
    const core::Brush b = fttBox();
    const core::BrushFace& f = *faceWith(b, 2, 1);
    const auto poly = faceUvPolygon(b, f, kTex);
    const UvBounds bounds = *polygonsBounds({poly});
    const V2 c = boundsCentre(bounds);
    for (const char axis : {'u', 'v'}) {
        const FttResult r = transformFaces(b, {f.id}, mirrorXformAbout(axis, axis == 'u' ? c[0] : c[1]), kTex);
        const core::BrushFace* nf = nullptr;
        for (const core::BrushFace& x : r.brush.faces)
            if (x.id == f.id) nf = &x;
        const auto after = faceUvPolygon(r.brush, *nf, kTex);
        const UvBounds nb = *polygonsBounds({after});
        close(nb.min, bounds.min);
        close(nb.max, bounds.max);
        const size_t i = axis == 'u' ? 0 : 1;
        for (size_t k = 0; k < poly.size(); ++k) CHECK(after[k][i] == doctest::Approx(bounds.min[i] + bounds.max[i] - poly[k][i]).epsilon(1e-6));
    }
    const core::Brush once = transformFaces(b, {f.id}, mirrorXformAbout('u', c[0]), kTex).brush;
    const core::Brush twice = transformFaces(once, {f.id}, mirrorXformAbout('u', c[0]), kTex).brush;
    const core::BrushFace* tf = nullptr;
    for (const core::BrushFace& x : twice.faces)
        if (x.id == f.id) tf = &x;
    const auto back = faceUvPolygon(twice, *tf, kTex);
    for (size_t k = 0; k < poly.size(); ++k) close(back[k], poly[k]);
}

TEST_CASE("ftt: align edge rotates the nearest axis onto the edge about its midpoint") {
    const V2 a{0, 0}, c{std::cos(0.35), std::sin(0.35)};
    const auto t = alignEdgeXform(a, c);
    REQUIRE(t);
    const V2 p = applyXform(*t, a), q = applyXform(*t, c);
    CHECK(q[1] - p[1] == doctest::Approx(0).epsilon(1e-9));
    CHECK(std::hypot(q[0] - p[0], q[1] - p[1]) == doctest::Approx(1).epsilon(1e-9));
    close(applyXform(*t, {(a[0] + c[0]) / 2, (a[1] + c[1]) / 2}), {(a[0] + c[0]) / 2, (a[1] + c[1]) / 2});
    const auto vert = alignEdgeXform({0, 0}, {0.1, 1});
    REQUIRE(vert);
    CHECK(applyXform(*vert, {0.1, 1})[0] - applyXform(*vert, {0, 0})[0] == doctest::Approx(0).epsilon(1e-9));
    CHECK(*alignEdgeXform({0, 0}, {1, 0}) == kIdentity);
    CHECK_FALSE(alignEdgeXform({1, 1}, {1, 1}));
}

TEST_CASE("ftt: unwrap modes re-project each face; follow needs two faces") {
    const core::Brush b = fttBox();
    const std::vector<uint32_t> ids = {faceWith(b, 2, 1)->id, faceWith(b, 0, 1)->id};
    CHECK(unwrapFaces(b, ids, UnwrapMode::Ortho, kTex).changed == 2);
    CHECK(unwrapFaces(b, ids, UnwrapMode::Axial, kTex).changed == 2);
    CHECK(unwrapFaces(b, {ids[0]}, UnwrapMode::Follow, kTex).changed == 0);
    CHECK(unwrapFaces(b, {999}, UnwrapMode::Ortho, kTex).changed == 0);
}

TEST_CASE("ftt: the ideal shape is the face extent in metres over the natural size, whatever the current scale") {
    const core::Brush b = fttBox();
    const core::BrushFace& f = *faceWith(b, 2, 1);
    close(idealSize(b, f, kTex), {4, 2});
    const core::Brush scaled = transformFaces(b, {f.id}, scaleXformAbout(3, 0.5, {0, 0}), kTex).brush;
    for (const core::BrushFace& x : scaled.faces)
        if (x.id == f.id) close(idealSize(scaled, x, kTex), {4, 2}, 1e-5);
}

TEST_CASE("ftt: drag maths") {
    const UvBounds bounds{{1, 2}, {3, 4}};
    const DragOpts o;
    close(applyXform(dragXform(Handle::Move, {2, 3}, {2.3, 3.4}, bounds, o), {1, 2}), {1.3, 2.4});
    close(applyXform(dragXform(Handle::Move, {2, 3}, {2.3, 3.4}, bounds, {0.25, false}), {1, 2}), {1.25, 2.5});
    const Xform e = dragXform(Handle::E, {3, 3}, {4, 3.5}, bounds, o);   // east handle: u 3 -> 4 over a width of 2 => x1.5; v untouched
    close(applyXform(e, {1, 2}), {1, 2});
    close(applyXform(e, {3, 4}), {4, 4});
    const Xform nw = dragXform(Handle::NW, {1, 2}, {0, 1}, bounds, o);   // pivot = se corner
    close(applyXform(nw, {3, 4}), {3, 4});
    close(applyXform(nw, {1, 2}), {0, 1});
    const Xform uni = dragXform(Handle::SE, {3, 4}, {5, 4}, bounds, {0, true});   // width x2, aspect lock => height x2
    close(applyXform(uni, {3, 4}), {5, 6});
    close(applyXform(uni, {1, 2}), {1, 2});
    const Xform flip = dragXform(Handle::E, {3, 3}, {0.5, 3}, bounds, o);   // dragged past the west edge: mirrored, not collapsed
    CHECK(applyXform(flip, {3, 3})[0] < 1);
    const V2 c = boundsCentre(bounds);
    const Xform t = dragXform(Handle::Rotate, {c[0] + 1, c[1]}, {c[0], c[1] + 1}, bounds, o);
    close(applyXform(t, {c[0] + 1, c[1]}), {c[0], c[1] + 1});
    const Xform s = dragXform(Handle::Rotate, {c[0] + 1, c[1]}, {c[0] + std::cos(0.4), c[1] + std::sin(0.4)}, bounds, {0, true});
    const V2 q = applyXform(s, {c[0] + 1, c[1]});
    CHECK(std::atan2(q[1] - c[1], q[0] - c[0]) * 180 / 3.14159265358979323846 == doctest::Approx(30).epsilon(1e-6));   // 22.9 deg snaps to 30
}

TEST_CASE("ftt: hit tests, point in polygon, nearest edge") {
    const std::vector<V2> poly = {{1, 2}, {3, 2}, {3, 4}, {1, 4}};
    const UvBounds bounds = *polygonsBounds({poly});
    CHECK(hitTest(bounds, {poly}, {3.01, 4.01}, 0.05, 0.4) == Handle::SE);
    CHECK(hitTest(bounds, {poly}, {2, 1.6}, 0.05, 0.4) == Handle::Rotate);
    CHECK(hitTest(bounds, {poly}, {2, 3}, 0.05, 0.4) == Handle::Move);
    CHECK(hitTest(bounds, {poly}, {9, 9}, 0.05, 0.4) == Handle::None);
    CHECK(handlePoint(bounds, Handle::Rotate, 0.4) == V2{2, 1.6});
    CHECK(pointInPolygon({2, 3}, poly));
    CHECK_FALSE(pointInPolygon({0, 3}, poly));
    const auto edge = nearestEdge({poly}, {2, 2.02}, 0.1);
    REQUIRE(edge);
    CHECK((*edge)[0] == V2{1, 2});
    CHECK((*edge)[1] == V2{3, 2});
    CHECK_FALSE(nearestEdge({poly}, {2, 3}, 0.1));
}
