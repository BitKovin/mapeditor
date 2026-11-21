// T11.3c: the texturing actions on the document (ports of the texturing tests of shell/tests/tools.test.ts, copySurface.test.ts, instanceActions.test.ts (texture keys),
// surfaceInspector.test.ts (repeat override, hotspot button) and batch33.test.ts (a texture replaces a layout)). Pure, no window, no GPU.
#include <doctest.h>

#include <cmath>
#include <map>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/texturing/face_edit.h"
#include "tools/texturing/inspector.h"

using namespace native;
using namespace native::tools::tex;
using doc::SelRef;
using native::tools::BrushEdit;
namespace tp = core::texplan;

namespace {

std::shared_ptr<const doc::DocumentVersion> makeDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b1", {0, 0, 0}, {4, 2, 1}, "dev/wall"));
    w.brushes.push_back(core::Brush::makeBox("b2", {10, 0, 0}, {14, 2, 1}, "dev/floor"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

const core::Brush& brushOf(const doc::DocumentVersion& d, const std::string& id) {
    for (const auto& n : d.entities[0]->brushes)
        if (n->brush.id == id) return n->brush;
    throw std::runtime_error("no brush");
}
const core::BrushFace& faceOf(const doc::DocumentVersion& d, const std::string& b, uint32_t id) {
    for (const core::BrushFace& f : brushOf(d, b).faces)
        if (f.id == id) return f;
    throw std::runtime_error("no face");
}

const tp::TexInfo& info(const std::string& path, int w, int h) {
    static std::map<std::string, tp::TexInfo> m;
    auto it = m.find(path);
    if (it == m.end()) {
        tp::TexInfo i;
        i.path = path;
        i.width = w;
        i.height = h;
        i.ppm = 64;
        i.props = tp::defaultProps();
        it = m.emplace(path, i).first;
    }
    return it->second;
}
const InfoFn kNone{};
const InfoFn kLoaded = [](const std::string& p) -> const tp::TexInfo* { return &info(p, 256, 128); };

std::vector<SelRef> faceSel(const std::string& b, uint32_t f) { return {SelRef::face("w", b, f)}; }
std::vector<SelRef> brushSel(const std::string& b) { return {SelRef::brush("w", b)}; }

} // namespace

TEST_CASE("faceTargets: a face, a brush, or a whole entity; instances and missing things are skipped") {
    const auto d = makeDoc();
    CHECK(faceTargets(*d, {SelRef::face("w", "b1", 3)}).size() == 1);
    CHECK(faceTargets(*d, brushSel("b1")).size() == 6);
    CHECK(faceTargets(*d, {SelRef::entity("w")}).size() == 12);
    CHECK(faceTargets(*d, {SelRef::brush("w", "b1"), SelRef::face("w", "b1", 2)}).size() == 6);   // no duplicates
    CHECK(faceTargets(*d, {SelRef::instance("i")}).empty());
    CHECK(faceTargets(*d, {SelRef::brush("w", "nope")}).empty());
}

TEST_CASE("apply texture: the selected face only in face mode, otherwise the whole brush; the same texture again is no edit") {
    const auto d = makeDoc();
    BrushEdit r = applyTexture(d, brushSel("b1"), "dev/wood", kNone);
    REQUIRE(r.ok());
    for (const core::BrushFace& f : brushOf(*r.version, "b1").faces) CHECK(f.texture == "dev/wood");
    CHECK(brushOf(*r.version, "b2").faces[0].texture == "dev/floor");
    BrushEdit one = applyTexture(r.version, faceSel("b1", 2), "dev/metal", kNone);
    REQUIRE(one.ok());
    int metal = 0;
    for (const core::BrushFace& f : brushOf(*one.version, "b1").faces) metal += f.texture == "dev/metal";
    CHECK(metal == 1);
    CHECK(faceOf(*one.version, "b1", 2).texture == "dev/metal");
    const BrushEdit again = applyTexture(r.version, brushSel("b1"), "dev/wood", kNone);
    CHECK_FALSE(again.ok());
    CHECK(again.message.empty());   // nothing changed is not an error
}

TEST_CASE("apply texture: a face without uvs gets the texture at its natural size from its bottom-left corner; a face with a projection keeps its stretch") {
    const auto d = makeDoc();
    const BrushEdit r = applyTexture(d, faceSel("b1", 1), "dev/new", kLoaded);   // 256 x 128 px = 4 x 2 m
    REQUIRE(r.ok());
    const core::BrushFace& f = faceOf(*r.version, "b1", 1);
    CHECK(f.texture == "dev/new");
    REQUIRE(f.uvs.size() == f.loop.size());
    CHECK(projectionOfFace(f, kLoaded).scale == V2{4, 2});
}

TEST_CASE("apply texture: a face with a projection keeps its stretch relative to the natural size and its shift in texels") {
    const auto d = makeDoc();
    const InfoFn sizes = [](const std::string& p) -> const tp::TexInfo* { return p == "dev/small" ? &info("dev/small", 128, 128) : &info("dev/wall", 256, 128); };
    const BrushEdit s = editProjection(d, faceSel("b1", 1), "Scale", [](const Projection& p, const core::Brush&, const core::BrushFace&) {
        Projection q = p;
        q.scale = {8, 2};
        q.shift = {0.5, 0.5};
        return q;
    }, sizes);
    REQUIRE(s.ok());
    const BrushEdit r = applyTexture(s.version, faceSel("b1", 1), "dev/small", sizes);   // natural 2 x 2 m instead of 4 x 2: kx = 0.5, ky = 1
    REQUIRE(r.ok());
    const Projection p = projectionOfFace(faceOf(*r.version, "b1", 1), sizes);
    CHECK(p.scale == V2{4, 2});
    CHECK(p.shift == V2{1, 0.5});   // 0.5 * 256 px / 128 px, 0.5 * 128 / 128
    CHECK(faceOf(*r.version, "b1", 1).texture == "dev/small");
}

TEST_CASE("apply texture removes the texture group layout of the face") {
    const auto d = makeDoc();
    // give face 1 a layout
    doc::VersionEditor ed(d);
    core::Brush b = brushOf(*d, "b1");
    for (core::BrushFace& f : b.faces)
        if (f.id == 1) {
            f.look = std::make_shared<const nlohmann::json>(nlohmann::json{{"layout", {{"group", "bld/apartment"}}}});
            refreshLookHash(f);
        }
    ed.replaceBrush("w", b);
    const auto with = ed.finish();
    REQUIRE(with);
    CHECK(tp::faceKey(faceOf(*with, "b1", 1), "layout"));
    const BrushEdit r = applyTexture(with, faceSel("b1", 1), "dev/wall", kNone);   // the same texture: the layout still goes
    REQUIRE(r.ok());
    CHECK_FALSE(tp::faceKey(faceOf(*r.version, "b1", 1), "layout"));
    CHECK(faceOf(*r.version, "b1", 1).lookHash == 0);
}

TEST_CASE("projection edits write both the parameters and the resulting uvs; fit and reset follow") {
    const auto d = makeDoc();
    const BrushEdit s = editProjection(d, faceSel("b1", 1), "Scale", [](const Projection& p, const core::Brush&, const core::BrushFace&) {
        Projection q = p;
        q.scale = {1, 1};
        return q;
    }, kNone);
    REQUIRE(s.ok());
    const core::BrushFace& f = faceOf(*s.version, "b1", 1);
    CHECK(f.projection.scaleU == 1);
    CHECK(f.projection.shiftU == 0);
    CHECK(f.uvs.size() == 4);
    CHECK(f.lookHash != 0);
    const BrushEdit fit = fitTexture(s.version, faceSel("b1", 1), kNone);
    REQUIRE(fit.ok());
    CHECK(faceOf(*fit.version, "b1", 1).projection.scaleU != 1);
    const BrushEdit reset = resetProjection(fit.version, faceSel("b1", 1), kNone);
    REQUIRE(reset.ok());
    const core::BrushFace& rf = faceOf(*reset.version, "b1", 1);
    CHECK(rf.projection.scaleU == 2);
    double umin = 1e300, vmax = -1e300;   // the texture's bottom-left corner sits on the face's bottom-left corner
    for (const core::Vec2& q : rf.uvs) {
        umin = std::min(umin, q.u);
        vmax = std::max(vmax, q.v);
    }
    CHECK(umin == doctest::Approx(0).epsilon(1e-9));
    CHECK(vmax == doctest::Approx(1).epsilon(1e-9));
}

TEST_CASE("lightmap density: stored on the faces, clamped, no-op edits are no edit") {
    const auto d = makeDoc();
    const auto times = [](double k) { return [k](double v) { return v * k; }; };
    BrushEdit a = editLightmapScale(d, faceSel("b1", 1), "Lightmap density", times(2));
    REQUIRE(a.ok());
    CHECK(faceOf(*a.version, "b1", 1).lightmapScale == 2);
    CHECK(faceOf(*a.version, "b1", 0).lightmapScale == 1);
    a = editLightmapScale(a.version, faceSel("b1", 1), "Lightmap density", [](double) { return 1000.0; });
    CHECK(faceOf(*a.version, "b1", 1).lightmapScale == 16);
    a = editLightmapScale(a.version, faceSel("b1", 1), "Lightmap density", [](double) { return 0.001; });
    CHECK(faceOf(*a.version, "b1", 1).lightmapScale == 0.0625);
    CHECK_FALSE(editLightmapScale(a.version, faceSel("b1", 1), "Lightmap density", [](double) { return 0.0625; }).ok());
    const BrushEdit all = editLightmapScale(a.version, brushSel("b1"), "Lightmap density", [](double) { return 4.0; });
    REQUIRE(all.ok());
    for (const core::BrushFace& f : brushOf(*all.version, "b1").faces) CHECK(f.lightmapScale == 4);
}

TEST_CASE("copy UV onto another face: texture and parameters, uvs recomputed, one edit; the same face and missing faces are refused") {
    const auto d = makeDoc();
    const BrushEdit s = editProjection(d, faceSel("b1", 1), "Scale", [](const Projection& p, const core::Brush&, const core::BrushFace&) {
        Projection q = p;
        q.scale = {3, 5};
        q.shift = {0.25, 0.5};
        q.rotate = 30;
        return q;
    }, kNone);
    REQUIRE(s.ok());
    const BrushEdit c = copySurface(s.version, {"w", "b1", 1}, {"w", "b2", 1}, kNone);
    REQUIRE(c.ok());
    const core::BrushFace& t = faceOf(*c.version, "b2", 1);
    CHECK(t.texture == "dev/wall");
    const Projection p = projectionOfFace(t, kNone);
    CHECK(p.scale == V2{3, 5});
    CHECK(p.shift == V2{0.25, 0.5});
    CHECK(p.rotate == 30);
    const auto expect = projectFace(brushOf(*c.version, "b2"), t, p);
    REQUIRE(t.uvs.size() == expect.size());
    for (size_t i = 0; i < expect.size(); ++i) CHECK(t.uvs[i].u == doctest::Approx(expect[i][0]));
    CHECK(faceOf(*c.version, "b2", 2).texture == "dev/floor");
    CHECK_FALSE(copySurface(s.version, {"w", "b1", 1}, {"w", "b1", 1}, kNone).ok());
    CHECK_FALSE(copySurface(s.version, {"w", "b1", 1}, {"w", "nope", 1}, kNone).ok());
    // paste: the copied face onto a whole brush, the copied face itself is skipped
    size_t n = 0;
    const BrushEdit paste = pasteUv(s.version, {"w", "b1", 1}, brushSel("b2"), kNone, &n);
    REQUIRE(paste.ok());
    CHECK(n == 6);
    for (const core::BrushFace& f : brushOf(*paste.version, "b2").faces) CHECK(f.texture == "dev/wall");
    size_t m = 0;
    const BrushEdit self = pasteUv(s.version, {"w", "b1", 1}, brushSel("b1"), kNone, &m);
    CHECK(m == 5);
    CHECK_FALSE(pasteUv(s.version, {"w", "gone", 1}, brushSel("b2"), kNone).ok());
}

TEST_CASE("flow texture action: across a shared edge of one brush; other brushes and the same face are refused with a message") {
    const auto d = makeDoc();
    const core::Brush& b = brushOf(*d, "b1");
    const core::BrushFace& a = b.faces[0];
    const core::BrushFace* nb = nullptr;
    for (const core::BrushFace& f : b.faces) {
        if (f.id == a.id) continue;
        int shared = 0;
        for (uint32_t v : f.loop) shared += std::find(a.loop.begin(), a.loop.end(), v) != a.loop.end();
        if (shared == 2) nb = &f;
    }
    REQUIRE(nb);
    const BrushEdit r = flowTexture(d, {"w", "b1", a.id}, {"w", "b1", nb->id}, kNone);
    REQUIRE(r.ok());
    CHECK_FALSE(faceOf(*r.version, "b1", nb->id).uvs.empty());
    CHECK(flowTexture(d, {"w", "b1", a.id}, {"w", "b2", 1}, kNone).message.find("same brush") != std::string::npos);
    CHECK(flowTexture(d, {"w", "b1", a.id}, {"w", "b1", a.id}, kNone).message.find("different face") != std::string::npos);
}

TEST_CASE("lift material returns the texture of the face, or nothing") {
    const auto d = makeDoc();
    CHECK(*liftMaterial(*d, {"w", "b2", 1}) == "dev/floor");
    CHECK_FALSE(liftMaterial(*d, {"w", "b2", 99}));
    CHECK_FALSE(liftMaterial(*d, {"nope", "b2", 1}));
}

TEST_CASE("texture keys (Radiant): shift, scale and rotate by the steps, Alt is 8x finer, per face or per brush") {
    const auto d = makeDoc();
    const TexSteps st;
    BrushEdit s = nudgeTexture(d, brushSel("b1"), NudgeMode::Shift, TexKey::Left, false, st, kNone);
    REQUIRE(s.ok());
    for (const core::BrushFace& f : brushOf(*s.version, "b1").faces) {
        CHECK(projectionOfFace(f, kNone).shift == V2{0.125, 0});
        CHECK_FALSE(f.uvs.empty());   // uvs are rewritten so Core / export see the result
    }
    s = nudgeTexture(s.version, brushSel("b1"), NudgeMode::Shift, TexKey::Down, false, st, kNone);
    CHECK(projectionOfFace(brushOf(*s.version, "b1").faces[0], kNone).shift == V2{0.125, -0.125});
    s = nudgeTexture(s.version, brushSel("b1"), NudgeMode::Shift, TexKey::Right, true, st, kNone);
    CHECK(projectionOfFace(brushOf(*s.version, "b1").faces[0], kNone).shift[0] == doctest::Approx(0.125 - 0.125 / 8));
    // a selected face is the only one that changes
    const BrushEdit up = nudgeTexture(d, faceSel("b1", 3), NudgeMode::Shift, TexKey::Up, false, st, kNone);
    REQUIRE(up.ok());
    for (const core::BrushFace& f : brushOf(*up.version, "b1").faces) CHECK(projectionOfFace(f, kNone).shift[1] == (f.id == 3 ? 0.125 : 0));
    // scale and rotate
    BrushEdit k = nudgeTexture(d, faceSel("b1", 1), NudgeMode::Scale, TexKey::Right, false, st, kNone);
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).scale[0] == doctest::Approx(2 * 1.1));
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).scale[1] == 2);
    k = nudgeTexture(k.version, faceSel("b1", 1), NudgeMode::Scale, TexKey::Left, false, st, kNone);
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).scale[0] == doctest::Approx(2));
    k = nudgeTexture(k.version, faceSel("b1", 1), NudgeMode::Scale, TexKey::Up, false, st, kNone);
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).scale[1] == doctest::Approx(2.2));
    k = nudgeTexture(k.version, faceSel("b1", 1), NudgeMode::Rotate, TexKey::RotCw, false, st, kNone);
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).rotate == 15);
    k = nudgeTexture(k.version, faceSel("b1", 1), NudgeMode::Rotate, TexKey::RotCcw, true, st, kNone);
    CHECK(projectionOfFace(faceOf(*k.version, "b1", 1), kNone).rotate == doctest::Approx(15 - 15.0 / 8));
    CHECK_FALSE(nudgeTexture(d, {}, NudgeMode::Shift, TexKey::Left, false, st, kNone).ok());   // nothing selected
}

TEST_CASE("face repetition override: buttons write look.repeat, clearing removes it, an empty override is removed") {
    const auto d = makeDoc();
    RepeatPatch mirror;
    mirror.setMode = true;
    mirror.mode = "mirror";
    BrushEdit r = setFaceRepeat(d, faceSel("b1", 1), 0, mirror);
    REQUIRE(r.ok());
    const nlohmann::json* rep = tp::faceKey(faceOf(*r.version, "b1", 1), "repeat");
    REQUIRE(rep);
    CHECK(*rep == nlohmann::json::parse(R"({"u":{"mode":"mirror"}})"));
    RepeatPatch clamp;
    clamp.setEdge = true;
    clamp.edge = "clamp";
    r = setFaceRepeat(r.version, faceSel("b1", 1), 1, clamp);
    CHECK(*tp::faceKey(faceOf(*r.version, "b1", 1), "repeat") == nlohmann::json::parse(R"({"u":{"mode":"mirror"},"v":{"edge":"clamp"}})"));
    RepeatPatch clearMode;
    clearMode.setMode = true;
    r = setFaceRepeat(r.version, faceSel("b1", 1), 0, clearMode);
    RepeatPatch clearEdge;
    clearEdge.setEdge = true;
    r = setFaceRepeat(r.version, faceSel("b1", 1), 1, clearEdge);
    REQUIRE(r.ok());
    CHECK_FALSE(tp::faceKey(faceOf(*r.version, "b1", 1), "repeat"));
    CHECK(faceOf(*r.version, "b1", 1).lookHash == 0);
}

TEST_CASE("hotspot action: applies the best-fitting hotspot of the texture; says so when there are none") {
    const auto d = makeDoc();
    const HotspotFn none = [](const std::string&) -> std::optional<HotspotSet> { return std::nullopt; };
    const BrushEdit no = applyHotspot(d, faceSel("b1", 1), none);
    CHECK_FALSE(no.ok());
    CHECK(no.message.find("no hotspots") != std::string::npos);
    const std::vector<Hotspot> spots = {{0, 0, 1, 0.25}, {0, 0.25, 0.5, 0.75}};
    const HotspotFn have = [&](const std::string&) -> std::optional<HotspotSet> { return HotspotSet{spots, {256, 128, 64}}; };   // 4 x 2 m
    const BrushEdit r = applyHotspot(d, faceSel("b1", 1), have);
    REQUIRE(r.ok());
    const core::BrushFace& f = faceOf(*r.version, "b1", 1);
    const int k = chooseHotspot(spots, {256, 128, 64}, faceSize(brushOf(*r.version, "b1"), f));
    const Hotspot& hs = spots[static_cast<size_t>(k)];
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (const core::Vec2& q : f.uvs) {
        u0 = std::min(u0, q.u);
        u1 = std::max(u1, q.u);
        v0 = std::min(v0, q.v);
        v1 = std::max(v1, q.v);
    }
    CHECK(u1 - u0 == doctest::Approx(hs.w).epsilon(0.05));
    CHECK(v1 - v0 == doctest::Approx(hs.h).epsilon(0.05));
    CHECK(applyHotspot(d, {}, have).message.find("Select") != std::string::npos);
}

TEST_CASE("surface fields: the values of the first target face, the mixed flags, the repeat overrides and the lightmap density") {
    auto d = makeDoc();
    CHECK_FALSE(surfaceFields(*d, {}, kLoaded).valid);
    // a face without a projection shows the texture's natural size, no shift, no rotation
    SurfaceFields f = surfaceFields(*d, faceSel("b1", 1), kLoaded);
    REQUIRE(f.valid);
    CHECK(f.faceCount == 1);
    CHECK(f.texture == "dev/wall");
    CHECK(f.texels[0] == doctest::Approx(256));
    CHECK(f.stretch[0] == doctest::Approx(1));
    CHECK(f.rotate == doctest::Approx(0));
    CHECK(f.lightmap == doctest::Approx(1));
    CHECK(f.repeatMode[0].empty());
    // after edits the fields follow
    BrushEdit e = nudgeTexture(d, brushSel("b1"), NudgeMode::Shift, TexKey::Right, false, TexSteps{}, kLoaded);
    REQUIRE(e.ok());
    d = e.version;
    e = editLightmapScale(d, faceSel("b1", 1), "x", [](double v) { return v * 2; });
    REQUIRE(e.ok());
    d = e.version;
    e = setFaceRepeat(d, faceSel("b1", 1), 0, RepeatPatch{true, true, "mirror", "clamp"});
    REQUIRE(e.ok());
    d = e.version;
    f = surfaceFields(*d, faceSel("b1", 1), kLoaded);
    CHECK(f.shift[0] != doctest::Approx(0));
    CHECK(f.lightmap == doctest::Approx(2));
    CHECK(f.repeatMode[0] == "mirror");
    CHECK(f.edge[0] == "clamp");
    CHECK(f.repeatMode[1].empty());
    // a brush: every face is a target, the others differ in the lightmap density
    f = surfaceFields(*d, brushSel("b1"), kLoaded);
    CHECK(f.faceCount == 6);
    CHECK(f.mixedLightmap);
    // two brushes with different textures
    f = surfaceFields(*d, {SelRef::brush("w", "b1"), SelRef::brush("w", "b2")}, kLoaded);
    CHECK(f.faceCount == 12);
    CHECK(f.mixedTexture);
    // an unloaded texture has no density readout
    CHECK(surfaceFields(*d, faceSel("b1", 1), kNone).density.empty());
}
