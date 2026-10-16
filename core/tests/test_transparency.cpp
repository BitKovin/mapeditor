// Transparency in light baking (REQUIREMENTS §23): texture properties, the alpha mask, the CPU tracer, and CPU / GPU agreement.
#include "core/albedo.h"
#include "core/lightbake.h"
#include "core/live_bake.h"
#include "core/textures.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <functional>
#include <map>

using namespace core;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Brush textured(Brush b, const std::string& tex) { for (auto& f : b.faces) f.texture = tex; return b; }

// An image from a pixel function, with its transparency resolved the way AlbedoLibrary does it.
std::shared_ptr<const AlbedoImage> image(int w, int h, const std::function<std::array<uint8_t, 4>(int, int)>& px, const std::string& props) {
    DecodedImage d;
    d.width = w;
    d.height = h;
    d.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto c = px(x, y);
            for (int k = 0; k < 4; ++k) d.rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(k)] = c[static_cast<size_t>(k)];
        }
    AlbedoImage a = makeAlbedoImage(d);
    a.transparency = resolveTransparency(props, a.alphaKind);
    return std::make_shared<const AlbedoImage>(std::move(a));
}
// stripes along u, symmetric under u -> 1 - u (so a mirrored projection on the far face sees the same holes): hole | solid | solid | hole
std::shared_ptr<const AlbedoImage> grate(const std::string& props = "") {
    return image(4, 1, [](int x, int) { return std::array<uint8_t, 4>{128, 128, 128, static_cast<uint8_t>(x == 0 || x == 3 ? 0 : 255)}; }, props);
}
std::shared_ptr<const AlbedoImage> glass(const std::string& props) {
    return image(2, 2, [](int, int) { return std::array<uint8_t, 4>{255, 0, 0, 255}; }, props);
}
AlbedoLoader loaderOf(std::map<std::string, std::shared_ptr<const AlbedoImage>> m) {
    return [m](const std::string& name) -> std::shared_ptr<const AlbedoImage> { const auto it = m.find(name); return it == m.end() ? nullptr : it->second; };
}

// A floor, and a pane 2 m above it (x, z in -2..2) with texture "pane", lit by a sun straight down.
struct PaneScene {
    std::vector<Entity> entities;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    PaneScene() {
        Entity sun = ent("sun", "light_directional");
        sun.angles = {90, 0, 0};  // forward (0, -1, 0)
        sun.keyvalues["brightness"] = "1";
        entities = {ent("w", "worldspawn", {textured(Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), "floor"), textured(Brush::makeBox("pane", {-2, 2, -2}, {2, 2.1, 2}), "pane")}), sun};
        Document doc;
        doc.entities = entities;
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = 4;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
    }
    OccluderMesh mesh(const AlbedoLoader& loader) const {
        OccluderMesh m = buildOccluders(surfaces, LightingClasses::defaults(), &layout);
        attachAlbedo(m, loader);
        return m;
    }
};
// fraction of the sky seen straight up from a grid of floor points under the pane
double litUnderPane(const TriangleBvh& bvh) {
    double lit = 0;
    int n = 0;
    for (int i = 0; i < 40; ++i)
        for (int k = 0; k < 40; ++k) {
            const Vec3 o{-1.95 + i * 0.1, 0.01, -1.95 + k * 0.1};
            lit += bvh.transmittance(o, {0, 1, 0}, 100).x;
            ++n;
        }
    return lit / n;
}
}  // namespace

TEST_CASE("transparency: the stored mode resolves per image; props are sanitized") {
    CHECK(resolveTransparency("", AlphaKind::None).mode == TransparencyMode::Opaque);
    CHECK(resolveTransparency("", AlphaKind::Cutout).mode == TransparencyMode::Cutout);  // auto, on / off alpha
    const Transparency g = resolveTransparency("", AlphaKind::Translucent);               // auto, half transparent alpha: glass
    CHECK(g.mode == TransparencyMode::Translucent);
    CHECK(g.opacity == 1.f);  // the painted alpha is the opacity
    CHECK(resolveTransparency(R"({"transparency":{"mode":"opaque"}})", AlphaKind::Translucent).mode == TransparencyMode::Opaque);
    CHECK(resolveTransparency(R"({"transparency":{"mode":"cutout"}})", AlphaKind::None).mode == TransparencyMode::Opaque);  // nothing to cut
    CHECK(resolveTransparency(R"({"transparency":{"mode":"cutout"}})", AlphaKind::Translucent).mode == TransparencyMode::Cutout);
    const Transparency t = resolveTransparency(R"({"transparency":{"mode":"translucent","opacity":0.3,"tint":0.25,"cutoff":0.9}})", AlphaKind::None);
    CHECK(t.mode == TransparencyMode::Translucent);
    CHECK(t.opacity == doctest::Approx(0.3));
    CHECK(t.tint == doctest::Approx(0.25));
    CHECK(t.cutoff == doctest::Approx(0.9));
    CHECK(resolveTransparency("not json", AlphaKind::Cutout).mode == TransparencyMode::Cutout);
    const auto clean = sanitizeTextureProps(nlohmann::json::parse(R"({"transparency":{"mode":"glass","opacity":7,"tint":"x"}})"));
    CHECK(clean["transparency"]["mode"] == "auto");
    CHECK(clean["transparency"]["opacity"] == 1.0);
    CHECK(!clean["transparency"].contains("tint"));
}

TEST_CASE("transparency: alpha is classified as none, on / off (cutout) or half transparent (glass)") {
    auto kind = [](std::vector<uint8_t> alphas) {
        std::vector<uint8_t> rgba;
        for (uint8_t a : alphas) rgba.insert(rgba.end(), {10, 20, 30, a});
        return classifyAlpha(rgba.data(), alphas.size());
    };
    CHECK(kind({255, 250, 241}) == AlphaKind::None);
    CHECK(kind({0, 255, 0, 255, 0, 255, 0, 128, 255}) == AlphaKind::Cutout);           // holes with one soft edge texel
    CHECK(kind({120, 120, 120, 255, 255}) == AlphaKind::Translucent);                  // glass panes and solid lead
    CHECK(kind({200, 200, 200, 200}) == AlphaKind::Translucent);                       // uniformly a bit see-through
    CHECK(kind({0, 0, 0, 100, 100, 255}) == AlphaKind::Translucent);                   // a lot of half transparency
    // a slightly see-through image keeps its alpha (it used to be dropped unless some texel was below 0.5)
    const auto faint = image(2, 2, [](int, int) { return std::array<uint8_t, 4>{200, 220, 255, 200}; }, "");
    CHECK(faint->hasAlpha());
    CHECK(faint->transparency.mode == TransparencyMode::Translucent);
}

TEST_CASE("transparency: the alpha mask is kept only for images with transparent texels, at full size up to the limit, nearest sampled") {
    CHECK(!grate()->mips.empty());
    CHECK(grate()->hasAlpha());
    CHECK(grate()->alphaWidth == 4);
    CHECK(grate()->alpha[0] == 0);
    CHECK(grate()->alpha[1] == 255);
    CHECK(!glass("")->hasAlpha());
    // a big image keeps a 1024 mask while its colour shrinks to kAlbedoMaxSize, and the mask stays binary
    const auto big = image(2048, 8, [](int x, int) { return std::array<uint8_t, 4>{0, 0, 0, static_cast<uint8_t>((x / 3) % 2 ? 255 : 0)}; }, "");
    CHECK(big->width == kAlbedoMaxSize);
    CHECK(big->alphaWidth == kAlphaMaskMaxSize);
    for (uint8_t a : big->alpha) CHECK((a == 0 || a == 255));
}

TEST_CASE("transparency (CPU tracer): opaque blocks, cutout holes let light through, translucent tints it once per pane") {
    PaneScene s;
    {
        const OccluderMesh m = s.mesh(loaderOf({{"pane", grate(R"({"transparency":{"mode":"opaque"}})")}}));
        CHECK(!m.hasTransparency());
        const TriangleBvh bvh(m);
        CHECK(litUnderPane(bvh) == 0.0);
    }
    {
        const OccluderMesh m = s.mesh(loaderOf({{"pane", grate()}}));  // auto -> cutout
        REQUIRE(m.hasTransparency());
        const TriangleBvh bvh(m);
        const double lit = litUnderPane(bvh);
        CHECK(lit > 0.35);
        CHECK(lit < 0.65);
        CHECK(bvh.transmittance({3, 0.01, 3}, {0, 1, 0}, 100).x == 1.0);  // beside the pane
        // occluded() and closestHit agree with the holes
        int holes = 0, passes = 0;
        for (int i = 0; i < 40; ++i) {
            const Vec3 o{-1.95 + i * 0.1, 0.01, 0.3};
            const bool open = bvh.transmittance(o, {0, 1, 0}, 100).x > 0.5;
            holes += open;
            passes += !bvh.occluded(o, {0, 1, 0}, 100);
            CHECK((bvh.closestHit(o, {0, 1, 0}, 100).t < 0) == open);
        }
        CHECK(holes == passes);
        CHECK(holes > 0);
    }
    {
        const OccluderMesh m = s.mesh(loaderOf({{"pane", glass(R"({"transparency":{"mode":"translucent","opacity":0.5,"tint":1}})")}}));
        REQUIRE(m.hasTransparency());
        const TriangleBvh bvh(m);
        const Vec3 up = bvh.transmittance({0.3, 0.01, 0.7}, {0, 1, 0}, 100), down = bvh.transmittance({0.3, 3, 0.7}, {0, -1, 0}, 2.9);
        CHECK(up.x == doctest::Approx(0.5).epsilon(1e-3));  // one layer, not two (the pane has two faces)
        CHECK(up.y == doctest::Approx(0.0));
        CHECK(down.x == doctest::Approx(0.5).epsilon(1e-3));
        CHECK(!bvh.occluded({0.3, 0.01, 0.7}, {0, 1, 0}, 100));
        CHECK(bvh.closestHit({0.3, 0.01, 0.7}, {0, 1, 0}, 100).t > 0);  // a bounce ray sees the glass as a surface
        // no tint: white light, only dimmed
        const OccluderMesh m2 = s.mesh(loaderOf({{"pane", glass(R"({"transparency":{"mode":"translucent","opacity":0.25,"tint":0}})")}}));
        const Vec3 w = TriangleBvh(m2).transmittance({0.3, 0.01, 0.7}, {0, 1, 0}, 100);
        CHECK(w.x == doctest::Approx(0.75).epsilon(1e-3));
        CHECK(w.y == doctest::Approx(0.75).epsilon(1e-3));
    }
}

TEST_CASE("transparency (bake): CPU and GPU agree on cutout and coloured translucent shadows (skipped without a ray query device)") {
    PaneScene s;
    for (int variant = 0; variant < 2; ++variant) {
        CAPTURE(variant);
        BakeOptions opt;
        opt.gpuValidation = true;
        opt.bounces = 0;
        opt.superSamples = 1;
        opt.albedoLoader = loaderOf({{"pane", variant == 0 ? grate() : glass(R"({"transparency":{"mode":"translucent","opacity":0.5,"tint":1}})")}});
        BakeStats gst;
        const LightmapPages g = bakeLightmaps(s.layout, s.surfaces, s.lights, opt, &gst);
        BakeOptions cpu = opt;
        cpu.forceCpu = true;
        const LightmapPages c = bakeLightmaps(s.layout, s.surfaces, s.lights, cpu);
        // the CPU result itself: something under the pane is shadowed, and (glass) the light there is red
        double minG = 1e9, maxR = 0;
        for (int p = 0; p < c.pageCount; ++p)
            for (size_t t = 0; t < c.valid[static_cast<size_t>(p)].size(); ++t) {
                if (c.valid[static_cast<size_t>(p)][t] != 1) continue;
                minG = std::min<double>(minG, c.rgb[static_cast<size_t>(p)][t * 3 + 1]);
                maxR = std::max<double>(maxR, c.rgb[static_cast<size_t>(p)][t * 3]);
            }
        CHECK(minG < 0.05);
        CHECK(maxR > 0.3);
        if (!gst.usedGpu) { MESSAGE("no ray query device: GPU half skipped"); continue; }
        for (const auto& m : gst.validationMessages) CHECK(m.rfind("[note]", 0) == 0);
        size_t texels = 0, off = 0;
        for (int p = 0; p < g.pageCount; ++p)
            for (size_t t = 0; t < g.valid[static_cast<size_t>(p)].size(); ++t) {
                if (g.valid[static_cast<size_t>(p)][t] != 1) continue;
                ++texels;
                for (size_t ch = 0; ch < 3; ++ch)
                    if (std::fabs(g.rgb[static_cast<size_t>(p)][t * 3 + ch] - c.rgb[static_cast<size_t>(p)][t * 3 + ch]) > 0.02) { ++off; break; }
            }
        MESSAGE("variant " << variant << ": " << off << " of " << texels << " texels differ");
        CHECK(texels > 0);
        CHECK(static_cast<double>(off) / static_cast<double>(texels) < 0.02);  // texel-edge rounding (float vs double uv) only
    }
}

TEST_CASE("transparency (live baker): the GPU session honours the holes like the CPU (skipped without a ray query device)") {
    PaneScene s;
    auto bake = [&](bool gpu) {
        LiveSettings cfg;
        cfg.useGpu = gpu;
        cfg.gpuValidation = gpu;
        cfg.bounces = false;
        cfg.maxPasses = 1;
        cfg.directSupersample = 1;
        cfg.albedoLoader = loaderOf({{"pane", grate()}});
        auto b = std::make_unique<LiveBaker>(cfg);
        b->setGeometry(s.layout, s.surfaces);
        b->setLights(s.lights);
        const Vec3 cam{0, 5, 0};
        while (!b->converged()) b->step(&cam, 1 << 20);
        return b;
    };
    const auto c = bake(false), g = bake(true);
    if (!g->gpuActive() && g->evaluator() == "cpu") { MESSAGE("no GPU session: " << g->gpuError()); return; }
    MESSAGE("live evaluator: " << g->evaluator());
    for (const auto& m : g->gpuValidation()) CHECK(m.rfind("[note]", 0) == 0);
    REQUIRE(c->sampleCount() == g->sampleCount());
    size_t lit = 0, dark = 0, off = 0;
    for (size_t i = 0; i < c->sampleCount(); ++i) {
        const double a = c->value(i).x, b = g->value(i).x;
        lit += a > 0.5;
        dark += a < 0.05;
        off += std::fabs(a - b) > 0.02;
    }
    CHECK(lit > 0);
    CHECK(dark > 0);
    CHECK(static_cast<double>(off) / static_cast<double>(c->sampleCount()) < 0.02);
}

TEST_CASE("transparency (CPU tracer): half transparent coloured glass from its alpha (auto) passes coloured light, per texel") {
    PaneScene s;
    // stained glass: left half blue at alpha 0.25 (mostly clear), right half red at alpha 0.75 (dense); auto -> translucent, opacity = alpha
    const auto stained = image(4, 1, [](int x, int) { return x < 2 ? std::array<uint8_t, 4>{0, 0, 255, 64} : std::array<uint8_t, 4>{255, 0, 0, 191}; }, "");
    REQUIRE(stained->transparency.mode == TransparencyMode::Translucent);
    const OccluderMesh m = s.mesh(loaderOf({{"pane", stained}}));
    const TriangleBvh bvh(m);
    // sample many points under the pane: two kinds of light, blue and bright, or red and dim
    int blue = 0, red = 0;
    for (int i = 0; i < 40; ++i) {
        const Vec3 T = bvh.transmittance({-1.95 + i * 0.1, 0.01, 0.7}, {0, 1, 0}, 100);
        if (T.z > 0.5 && T.x < 1e-6) { ++blue; CHECK(T.z == doctest::Approx(1.0 - 64.0 / 255.0).epsilon(1e-3)); }
        else if (T.x > 0.1 && T.z < 1e-6) { ++red; CHECK(T.x == doctest::Approx(1.0 - 191.0 / 255.0).epsilon(1e-3)); }
        CHECK(T.y < 1e-6);  // neither glass lets green through
    }
    CHECK(blue > 5);
    CHECK(red > 5);
}

TEST_CASE("albedo UVs from the face's stored uvs (§24 C3): bounce colour and transparency sample where the editor shows the texture") {
    Brush b = Brush::makeBox("b", {0, 0, 0}, {2, 1, 1});
    BrushFace* top = nullptr;
    for (auto& f : b.faces) if (b.faceNormal(f).y > 0.99) top = &f;
    REQUIRE(top);
    // an affine map the axial projection could never give: rotated and sheared, u = 0.3 x + 0.1 z + 0.25, v = -0.2 x + 0.5 z + 0.75
    auto U = [](const Vec3& p) { return 0.3 * p.x + 0.1 * p.z + 0.25; };
    auto V = [](const Vec3& p) { return -0.2 * p.x + 0.5 * p.z + 0.75; };
    for (uint32_t i : top->loop) top->uvs.push_back({U(b.vertices[i]), V(b.vertices[i])});
    UvMap m;
    REQUIRE(faceUvMap(b, *top, m));
    const Vec3 q{1.3, 1, 0.4};
    CHECK(m.u[0] * q.x + m.u[1] * q.y + m.u[2] * q.z + m.u[3] == doctest::Approx(U(q)).epsilon(1e-9));
    CHECK(m.v[0] * q.x + m.v[1] * q.y + m.v[2] * q.z + m.v[3] == doctest::Approx(V(q)).epsilon(1e-9));
    // no uvs, or the wrong number of them: no map
    BrushFace bare = *top;
    bare.uvs.clear();
    CHECK(!faceUvMap(b, bare, m));
    bare.uvs = {{0, 0}};
    CHECK(!faceUvMap(b, bare, m));

    // through the document: the light surface carries the map and the occluder triangles' albedo coordinates follow it
    Document doc;
    Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes = {b};
    doc.entities = {w};
    const auto surfaces = collectSurfaces(doc, {});
    const LightSurface* s = nullptr;
    for (const auto& x : surfaces) if (x.faceId == top->id) s = &x;
    REQUIRE(s);
    CHECK(s->hasUvMap);
    const Vec2 a = albedoUvAt(*s, q);
    CHECK(a.u == doctest::Approx(U(q)).epsilon(1e-9));
    CHECK(a.v == doctest::Approx(V(q)).epsilon(1e-9));
    const LightSurface* side = nullptr;
    for (const auto& x : surfaces) if (x.faceId != top->id) side = &x;
    REQUIRE(side);
    CHECK(!side->hasUvMap);  // no uvs: the projection as before
    const LightmapLayout layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), LightmapSettings{});
    const OccluderMesh mesh = buildOccluders(surfaces, LightingClasses::defaults(), &layout);
    bool checked = false;
    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        const float* tri = &mesh.tris[t * 9];
        if (!(tri[1] > 0.999f && tri[4] > 0.999f && tri[7] > 0.999f)) continue;  // a triangle of the top face
        for (int c = 0; c < 3; ++c) {
            const Vec3 p{tri[c * 3], tri[c * 3 + 1], tri[c * 3 + 2]};
            CHECK(mesh.albedoUv[t * 6 + static_cast<size_t>(c) * 2] == doctest::Approx(U(p)).epsilon(1e-5));
            CHECK(mesh.albedoUv[t * 6 + static_cast<size_t>(c) * 2 + 1] == doctest::Approx(V(p)).epsilon(1e-5));
        }
        checked = true;
    }
    CHECK(checked);
}
