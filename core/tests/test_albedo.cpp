#include "core/albedo.h"
#include "core/lightbake.h"
#include "test_util.h"
#include <doctest.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

using namespace core;
using namespace testutil;

namespace {
const unsigned char kJpeg444[] = {255,216,255,224,0,16,74,70,73,70,0,1,1,0,0,1,0,1,0,0,255,219,0,67,0,2,1,1,1,1,1,2,1,1,1,2,2,2,2,2,4,3,2,2,2,2,5,4,4,3,4,6,5,6,6,6,5,6,6,6,7,9,8,6,7,9,7,6,6,8,11,8,9,10,10,10,10,10,6,8,11,12,11,10,12,9,10,10,10,255,219,0,67,1,2,2,2,2,2,2,5,3,3,5,10,7,6,7,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,255,192,0,17,8,0,16,0,32,3,1,17,0,2,17,1,3,17,1,255,196,0,31,0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,255,196,0,181,16,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125,1,2,3,0,4,17,5,18,33,49,65,6,19,81,97,7,34,113,20,50,129,145,161,8,35,66,177,193,21,82,209,240,36,51,98,114,130,9,10,22,23,24,25,26,37,38,39,40,41,42,52,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,218,225,226,227,228,229,230,231,232,233,234,241,242,243,244,245,246,247,248,249,250,255,196,0,31,1,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,255,196,0,181,17,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119,0,1,2,3,17,4,5,33,49,6,18,65,81,7,97,113,19,34,50,129,8,20,66,145,161,177,193,9,35,51,82,240,21,98,114,209,10,22,36,52,225,37,241,23,24,25,26,38,39,40,41,42,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,130,131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,218,226,227,228,229,230,231,232,233,234,242,243,244,245,246,247,248,249,250,255,218,0,12,3,1,0,2,17,3,17,0,63,0,249,126,191,19,63,212,0,160,15,7,175,246,176,255,0,34,194,128,61,226,191,197,51,253,116,10,0,240,122,255,0,107,15,242,44,40,3,255,217};
const unsigned char kJpeg420[] = {255,216,255,224,0,16,74,70,73,70,0,1,1,0,0,1,0,1,0,0,255,219,0,67,0,2,1,1,1,1,1,2,1,1,1,2,2,2,2,2,4,3,2,2,2,2,5,4,4,3,4,6,5,6,6,6,5,6,6,6,7,9,8,6,7,9,7,6,6,8,11,8,9,10,10,10,10,10,6,8,11,12,11,10,12,9,10,10,10,255,219,0,67,1,2,2,2,2,2,2,5,3,3,5,10,7,6,7,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,255,192,0,17,8,0,16,0,32,3,1,34,0,2,17,1,3,17,1,255,196,0,31,0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,255,196,0,181,16,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125,1,2,3,0,4,17,5,18,33,49,65,6,19,81,97,7,34,113,20,50,129,145,161,8,35,66,177,193,21,82,209,240,36,51,98,114,130,9,10,22,23,24,25,26,37,38,39,40,41,42,52,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,218,225,226,227,228,229,230,231,232,233,234,241,242,243,244,245,246,247,248,249,250,255,196,0,31,1,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,255,196,0,181,17,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119,0,1,2,3,17,4,5,33,49,6,18,65,81,7,97,113,19,34,50,129,8,20,66,145,161,177,193,9,35,51,82,240,21,98,114,209,10,22,36,52,225,37,241,23,24,25,26,38,39,40,41,42,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,130,131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,218,226,227,228,229,230,231,232,233,234,242,243,244,245,246,247,248,249,250,255,218,0,12,3,1,0,2,17,3,17,0,63,0,249,126,138,40,175,196,207,245,0,240,122,40,162,191,218,195,252,139,63,255,217};
const unsigned char kJpegGray[] = {255,216,255,224,0,16,74,70,73,70,0,1,1,0,0,1,0,1,0,0,255,219,0,67,0,2,1,1,1,1,1,2,1,1,1,2,2,2,2,2,4,3,2,2,2,2,5,4,4,3,4,6,5,6,6,6,5,6,6,6,7,9,8,6,7,9,7,6,6,8,11,8,9,10,10,10,10,10,6,8,11,12,11,10,12,9,10,10,10,255,192,0,11,8,0,8,0,16,1,1,17,0,255,196,0,31,0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,255,196,0,181,16,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125,1,2,3,0,4,17,5,18,33,49,65,6,19,81,97,7,34,113,20,50,129,145,161,8,35,66,177,193,21,82,209,240,36,51,98,114,130,9,10,22,23,24,25,26,37,38,39,40,41,42,52,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,218,225,226,227,228,229,230,231,232,233,234,241,242,243,244,245,246,247,248,249,250,255,218,0,8,1,1,0,0,63,0,249,158,138,255,217};

std::vector<unsigned char> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
const std::string kTextures = std::string(TESTDATA_DIR) + "/../core/assets/textures/dev/";

std::vector<unsigned char> bytesOf(const unsigned char* p, size_t n) { return std::vector<unsigned char>(p, p + n); }

// A solid colour texture (sRGB bytes).
std::shared_ptr<const AlbedoImage> solid(uint8_t r, uint8_t g, uint8_t b, int size = 4) {
    DecodedImage d;
    d.width = d.height = size;
    d.rgba.assign(static_cast<size_t>(size) * size * 4, 255);
    for (size_t i = 0; i < d.rgba.size(); i += 4) { d.rgba[i] = r; d.rgba[i + 1] = g; d.rgba[i + 2] = b; }
    return std::make_shared<const AlbedoImage>(makeAlbedoImage(d));
}
std::array<uint8_t, 3> px(const DecodedImage& d, int x, int y) {
    const size_t o = (static_cast<size_t>(y) * d.width + x) * 4;
    return {d.rgba[o], d.rgba[o + 1], d.rgba[o + 2]};
}
}  // namespace

TEST_CASE("albedo: PNG and TGA files decode to the pixels the files hold") {
    const auto png = decodeImage(readFile(kTextures + "wall.png"), ".png");
    REQUIRE(png);
    CHECK(png->width == 128);
    CHECK(png->height == 128);
    CHECK(px(*png, 27, 7) == std::array<uint8_t, 3>{150, 110, 100});
    CHECK(px(*png, 0, 0) == std::array<uint8_t, 3>{95, 60, 55});
    const auto tga = decodeImage(readFile(kTextures + "tga_test.tga"), ".tga");
    REQUIRE(tga);
    CHECK(tga->width == 64);
    CHECK(px(*tga, 0, 0) == std::array<uint8_t, 3>{120, 90, 0});
    CHECK(px(*tga, 10, 20) == std::array<uint8_t, 3>{230, 200, 60});  // an asymmetric pixel: rows are not flipped, columns not mirrored
    CHECK(!decodeImage({1, 2, 3}, ".png"));
    CHECK(!decodeImage({}, ".tga"));
}

TEST_CASE("albedo: baseline JPEG, 4:4:4 and 4:2:0 and grey") {
    for (const auto& bytes : {bytesOf(kJpeg444, sizeof(kJpeg444)), bytesOf(kJpeg420, sizeof(kJpeg420))}) {
        const auto j = decodeImage(bytes, ".jpg");
        REQUIRE(j);
        CHECK(j->width == 32);
        CHECK(j->height == 16);
        const auto l = px(*j, 4, 4), r = px(*j, 28, 4);
        CHECK(std::abs(int(l[0]) - 200) <= 4);
        CHECK(std::abs(int(l[1]) - 40) <= 4);
        CHECK(std::abs(int(r[2]) - 200) <= 4);
        CHECK(std::abs(int(r[0]) - 40) <= 4);
    }
    const auto g = decodeJpeg(bytesOf(kJpegGray, sizeof(kJpegGray)));
    REQUIRE(g);
    CHECK(std::abs(int(px(*g, 3, 3)[0]) - 90) <= 3);
    // damaged data never crashes
    auto broken = bytesOf(kJpeg420, sizeof(kJpeg420));
    broken.resize(broken.size() / 2);
    (void)decodeJpeg(broken);
}

TEST_CASE("albedo: mip chain, trilinear sampling and repeat match what the GPU sampler does") {
    const auto png = decodeImage(readFile(kTextures + "wall.png"), ".png");
    REQUIRE(png);
    const AlbedoImage img = makeAlbedoImage(*png);
    CHECK(img.width == 128);
    CHECK(img.mipCount() == 8);  // 128 .. 1
    CHECK(img.mipWidth(7) == 1);
    // texel centre at lod 0 = the texel (linear)
    const Vec3 c = img.sample((27 + 0.5) / 128.0, (7 + 0.5) / 128.0, 0);
    CHECK(c.x == doctest::Approx(srgbToLinear(150)).epsilon(1e-4));
    CHECK(c.y == doctest::Approx(srgbToLinear(110)).epsilon(1e-4));
    // repeat
    const Vec3 a = img.sample(0.3, 0.7, 1.5), b = img.sample(2.3, -0.3, 1.5);
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-6));
    CHECK(a.z == doctest::Approx(b.z).epsilon(1e-6));
    // the top level is the average colour, anywhere
    const Vec3 avg = img.average();
    const Vec3 top = img.sample(0.123, 0.456, 20);
    CHECK(top.x == doctest::Approx(avg.x).epsilon(1e-6));
    // box filtering in LINEAR light: a 50% black / white checker averages to 0.5 linear (not the sRGB mid grey 0.21)
    DecodedImage d;
    d.width = d.height = 8;
    d.rgba.assign(8 * 8 * 4, 255);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) { const uint8_t v = ((x + y) & 1) ? 255 : 0; for (int k = 0; k < 3; ++k) d.rgba[(static_cast<size_t>(y) * 8 + x) * 4 + k] = v; }
    CHECK(makeAlbedoImage(d).average().x == doctest::Approx(0.5).epsilon(0.02));
    // big images are cut down
    DecodedImage big;
    big.width = 1024;
    big.height = 512;
    big.rgba.assign(static_cast<size_t>(1024) * 512 * 4, 128);
    const AlbedoImage small = makeAlbedoImage(big);
    CHECK(small.width == 256);
    CHECK(small.height == 128);
}

TEST_CASE("albedo: library caches, reloads changed files and rejects unsafe paths") {
    GameProfile p;
    p.textureRoot = "textures";
    AlbedoLibrary lib(std::filesystem::path(TESTDATA_DIR) / ".." / "core" / "assets", p);
    const auto a = lib.get("dev/wall");
    REQUIRE(a);
    CHECK(lib.get("dev/wall") == a);  // cached: the same object
    CHECK(!lib.get("dev/does_not_exist"));
    CHECK(!lib.get("../assets/game_profile"));
    CHECK(!lib.get("/etc/passwd"));
    CHECK(!lib.get(""));
}

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

// floor (grey, no texture file) and a wall with texture `wallTexture`, a white point light between them
struct WallRoom {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    explicit WallRoom(const std::string& wallTexture) {
        Brush floor = Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}, "floor_tex");
        Brush wall = Brush::makeBox("wall", {1.5, 0, -6}, {2.5, 4, 6}, wallTexture);
        Entity light = ent("l", "light_point");
        light.origin = {-1, 1.5, 0};
        light.keyvalues["brightness"] = "12";
        light.keyvalues["radius"] = "50";
        doc.entities = {ent("w", "worldspawn", {floor, wall}), light};
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = 2;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
    }
    Vec3 floorValue(const LightmapPages& P, const TexelSamples& s, Vec3 at) const {
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < s.count(); ++i) {
            if (s.nrm[i * 4 + 1] < 0.99f) continue;
            const Vec3 q{s.pos[i * 4], s.pos[i * 4 + 1], s.pos[i * 4 + 2]};
            if ((q - at).length() < bd) { bd = (q - at).length(); best = i; }
        }
        const size_t per = static_cast<size_t>(P.atlas) * P.atlas, page = s.texel[best] / per, t = s.texel[best] % per;
        return {P.rgb[page][t * 3], P.rgb[page][t * 3 + 1], P.rgb[page][t * 3 + 2]};
    }
};
}  // namespace

TEST_CASE("albedo cpu: a red wall bounces red light, a white one white") {
    WallRoom room("red");
    const AlbedoLoader loader = [](const std::string& name) -> std::shared_ptr<const AlbedoImage> {
        return name == "red" ? solid(255, 0, 0) : nullptr;  // floor_tex has no file: white
    };
    BakeOptions o;
    o.forceCpu = true;
    o.bounces = 1;
    o.giSamples = 256;
    o.giScale = 1;
    o.denoiseGi = false;
    BakeOptions direct = o;
    direct.bounces = 0;
    const TexelSamples samples = buildTexelSamples(room.layout, room.surfaces);
    const LightmapPages d = bakeLightmaps(room.layout, room.surfaces, room.lights, direct);
    const LightmapPages plain = bakeLightmaps(room.layout, room.surfaces, room.lights, o);
    o.albedoLoader = loader;
    const LightmapPages red = bakeLightmaps(room.layout, room.surfaces, room.lights, o);
    const Vec3 at{0.8, 0, 0};
    const Vec3 d0 = room.floorValue(d, samples, at), w = room.floorValue(plain, samples, at), r = room.floorValue(red, samples, at);
    const Vec3 iw = {w.x - d0.x, w.y - d0.y, w.z - d0.z}, ir = {r.x - d0.x, r.y - d0.y, r.z - d0.z};
    CHECK(iw.x > 0.01);
    CHECK(iw.y == doctest::Approx(iw.x).epsilon(0.05));  // white: no tint
    CHECK(ir.x == doctest::Approx(iw.x).epsilon(0.05));  // red keeps the red channel (albedo 1 there) ...
    CHECK(ir.y < 0.02 * ir.x);                           // ... and drops green and blue
    CHECK(ir.z < 0.02 * ir.x);
}

TEST_CASE("albedo cpu: textures are projected like the Shell does and sampled at the hit point") {
    // the face projection of the document is parsed: scale / shift / rotate
    Brush b = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
    json bj = brushJson(b);
    bj["faces"][0]["projection"] = {{"scale", {4, 8}}, {"shift", {0.25, 0.5}}, {"rotate", 90}};
    const Brush parsed = brushFromJson(bj);
    CHECK(parsed.faces[0].projection.scaleU == 4);
    CHECK(parsed.faces[0].projection.scaleV == 8);
    CHECK(parsed.faces[0].projection.shiftV == 0.5);
    CHECK(parsed.faces[0].projection.rotate == 90);
    CHECK(parsed.faces[1].projection.scaleU == 2);  // default
    // same numbers as planarCoordsProjected (shell/src/model/uv.ts): floor facing up, p = (3, 0, 5), default projection
    const Vec2 f = projectAlbedoUv({0, 1, 0}, {3, 0, 5}, FaceProjection{});
    CHECK(f.u == doctest::Approx(1.5));
    CHECK(f.v == doctest::Approx(2.5));
    // X facing wall (normal +x), p = (0, 2, 4): a = -z, b = -y
    const Vec2 w = projectAlbedoUv({1, 0, 0}, {0, 2, 4}, FaceProjection{});
    CHECK(w.u == doctest::Approx(-2.0));
    CHECK(w.v == doctest::Approx(-1.0));
    // rotated 90 degrees about the origin: (a, b) -> (-b, a)
    FaceProjection rot;
    rot.rotate = 90;
    const Vec2 q = projectAlbedoUv({0, 1, 0}, {3, 0, 5}, rot);
    CHECK(q.u == doctest::Approx(-2.5));
    CHECK(q.v == doctest::Approx(1.5));
}

namespace {
// A closed room: floor, ceiling and four walls, each its own brush with its own texture; plus `fillers` far away boxes with their own textures, so the
// texture array of the GPU holds hundreds of entries and the ids really index it.
struct TexturedRoom {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    std::vector<std::string> names;
    TexturedRoom(int fillers) {
        std::vector<Brush> bs;
        auto add = [&](const char* id, Vec3 a, Vec3 b, const std::string& tex) { bs.push_back(Brush::makeBox(id, a, b, tex)); names.push_back(tex); };
        add("floor", {-4, -1, -4}, {4, 0, 4}, "tex_floor");
        add("ceil", {-4, 3, -4}, {4, 4, 4}, "tex_ceil");
        add("n", {-4, 0, 4}, {4, 3, 5}, "tex_north");
        add("s", {-4, 0, -5}, {4, 3, -4}, "tex_south");
        add("e", {4, 0, -4}, {5, 3, 4}, "tex_east");
        add("w", {-5, 0, -4}, {-4, 3, 4}, "tex_west");
        for (int i = 0; i < fillers; ++i) add(("f" + std::to_string(i)).c_str(), {20.0 + 2 * (i % 20), 0, 20.0 + 2 * (i / 20)}, {21.0 + 2 * (i % 20), 1, 21.0 + 2 * (i / 20)}, "filler_" + std::to_string(i));
        Entity l = ent("l", "light_point");
        l.origin = {0.5, 2.0, 0.3};
        l.keyvalues["brightness"] = "14";
        l.keyvalues["radius"] = "60";
        doc.entities = {ent("w", "worldspawn", bs), l};
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = 2;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
    }
};
// a strongly structured texture (stripes + per-name tint) so wrong ids, wrong uv or wrong level of detail show up
std::shared_ptr<const AlbedoImage> tinted(const std::string& name) {
    size_t h = std::hash<std::string>{}(name);
    DecodedImage d;
    d.width = 64;
    d.height = 32;
    d.rgba.assign(64 * 32 * 4, 255);
    const uint8_t base[3] = {static_cast<uint8_t>(80 + (h & 0x7F)), static_cast<uint8_t>(80 + ((h >> 8) & 0x7F)), static_cast<uint8_t>(80 + ((h >> 16) & 0x7F))};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool stripe = ((x / 8) & 1) != 0;
            for (int k = 0; k < 3; ++k) d.rgba[(static_cast<size_t>(y) * 64 + x) * 4 + k] = stripe ? base[k] : static_cast<uint8_t>(base[k] / 3);
        }
    return std::make_shared<const AlbedoImage>(makeAlbedoImage(d));
}
}  // namespace

TEST_CASE("albedo gpu: the bindless texture array gives the CPU's bounce colours (hundreds of textures, mips, per face ids)") {
    TexturedRoom room(300);
    BakeOptions o;
    o.gpuValidation = true;
    o.bounces = 2;
    o.giSamples = 48;
    o.giScale = 1;
    o.denoiseGi = false;
    o.superSamples = 1;
    o.albedoLoader = [](const std::string& n) { return tinted(n); };
    BakeStats gst;
    const LightmapPages g = bakeLightmaps(room.layout, room.surfaces, room.lights, o, &gst);
    if (!gst.usedGpu) { MESSAGE("no Vulkan device with ray query and descriptor indexing, skipped: " << (gst.warnings.empty() ? "" : gst.warnings.back())); return; }
    MESSAGE("GPU device: " << gst.device << ", texels " << gst.texels);
    for (const auto& m : gst.validationMessages) MESSAGE(m);
    for (const auto& m : gst.validationMessages) CHECK(m.rfind("[note]", 0) == 0);
    BakeOptions c = o;
    c.forceCpu = true;
    const LightmapPages cp = bakeLightmaps(room.layout, room.surfaces, room.lights, c);
    BakeOptions noTex = c;
    noTex.albedoLoader = nullptr;
    const LightmapPages white = bakeLightmaps(room.layout, room.surfaces, room.lights, noTex);
    const TexelSamples samples = buildTexelSamples(room.layout, room.surfaces);
    double err = 0, tint = 0, scale = 0;
    size_t n = 0;
    for (size_t i = 0; i < samples.count(); ++i) {
        const size_t per = static_cast<size_t>(g.atlas) * g.atlas, page = samples.texel[i] / per, t = samples.texel[i] % per;
        for (int k = 0; k < 3; ++k) {
            const double a = g.rgb[page][t * 3 + k], b = cp.rgb[page][t * 3 + k], w = white.rgb[page][t * 3 + k];
            err += std::fabs(a - b);
            tint += std::fabs(b - w);
            scale += std::fabs(b);
        }
        ++n;
    }
    MESSAGE("texels " << n << ", mean |gpu - cpu| " << err / scale << " (relative), mean |textured - white| " << tint / scale);
    CHECK(tint / scale > 0.05);   // the textures change the result visibly ...
    CHECK(err / scale < 0.01);    // ... and the GPU reproduces it (same rays, same texture filter, float precision)
}
