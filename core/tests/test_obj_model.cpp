// misc_model: OBJ models baked into the level (obj_model.h).
#include "core/bsp_compile.h"
#include "core/lighting_classes.h"
#include "core/obj_model.h"
#include "core/raytrace.h"
#include "core/textures.h"
#include "doctest.h"
#include "test_util.h"
#include <set>

using namespace core;
using namespace testutil;

namespace {

void put(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A unit quad (two triangles) in the XY plane facing +Z, with texture coordinates 0..1 and material `mat`.
std::string quadObj(const std::string& mat, const std::string& mtllib = "") {
    return (mtllib.empty() ? "" : "mtllib " + mtllib + "\n") +
           std::string("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n") + (mat.empty() ? "" : "usemtl " + mat + "\n") +
           "f 1/1/1 2/2/1 3/3/1 4/4/1\n";
}

Entity modelEntity(const std::string& id, const std::string& path, Vec3 origin = {}) {
    Entity e;
    e.id = id;
    e.classname = "misc_model";
    e.origin = origin;
    e.keyvalues["model_path"] = path;
    return e;
}

}  // namespace

TEST_CASE("obj: faces are triangulated, v is flipped to the editor convention, materials are collected") {
    const ObjModel m = parseObj(quadObj("wood"));
    REQUIRE(m.tris.size() == 2);
    REQUIRE(m.materials.size() == 1);
    CHECK(m.materials[0] == "wood");
    CHECK(m.tris[0].hasUv);
    CHECK(m.tris[0].uv[0].v == doctest::Approx(1.0));  // vt (0,0) is the bottom left of the image: row 1 from the top
    CHECK(m.tris[0].uv[2].v == doctest::Approx(0.0));
    CHECK(m.mx.x == doctest::Approx(1));
    CHECK(m.tris[1].n[0].z == doctest::Approx(1));
    const ObjModel neg = parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n");  // negative indices count from the end
    REQUIRE(neg.tris.size() == 1);
    CHECK(neg.tris[0].p[1].x == doctest::Approx(1));
    CHECK_FALSE(neg.tris[0].hasUv);
    CHECK(neg.tris[0].n[0].z == doctest::Approx(1));  // no normals in the file: the face normal
}

TEST_CASE("obj: textures come from the .mtl, relative to the game root, or from an image named like the material") {
    TempDir dir;
    put(dir.path / "models/props/crate.obj", quadObj("Crate", "crate.mtl"));
    put(dir.path / "models/props/crate.mtl", "newmtl Crate\nKd 1 1 1\nmap_Kd -s 1 1 1 tex\\crate_diffuse.png\n");
    put(dir.path / "models/props/tex/crate_diffuse.png", "x");
    const auto a = loadObjModel(dir.path, "models/props/crate.obj");
    REQUIRE(a);
    REQUIRE(a->textures.size() == 1);
    CHECK(a->textures[0] == "/models/props/tex/crate_diffuse");  // no 'textures/' root, no extension

    // no .mtl at all: the image is found by the material's name
    put(dir.path / "models/barrel.obj", quadObj("Barrel"));
    put(dir.path / "models/barrel.jpg", "x");
    const auto b = loadObjModel(dir.path, "models/barrel.obj");
    REQUIRE(b);
    CHECK(b->textures[0] == "/models/barrel");

    // an .mtl whose map_Kd file is missing falls back to the material name too; the name matching ignores case
    put(dir.path / "models/lamp.obj", quadObj("Glass", "lamp.mtl"));
    put(dir.path / "models/lamp.mtl", "newmtl Glass\nmap_Kd gone.png\n");
    put(dir.path / "models/GLASS.tga", "x");
    const auto c = loadObjModel(dir.path, "models/lamp.obj");
    REQUIRE(c);
    CHECK(c->textures[0] == "/models/GLASS");

    // nothing found: the material has no image (the surfaces then use the default texture)
    put(dir.path / "models/ghost.obj", quadObj("Nothing"));
    const auto d = loadObjModel(dir.path, "models/ghost.obj");
    REQUIRE(d);
    CHECK(d->textures[0].empty());
    CHECK_FALSE(d->complete());

    std::string err;
    CHECK_FALSE(loadObjModel(dir.path, "../outside.obj", &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(loadObjModel(dir.path, "models/missing.obj", &err));
}

TEST_CASE("textures: a name with a leading slash is relative to the game root") {
    TempDir dir;
    put(dir.path / "models/x/t.png", "png");
    put(dir.path / "textures/dev/a.png", "png");
    GameProfile p;
    p.textureRoot = "textures";
    p.textureExtensions = {".tga", ".png"};
    const auto rooted = readTexture(dir.path, p, "/models/x/t");
    REQUIRE(rooted);
    CHECK(rooted->ext == ".png");
    CHECK(readTexture(dir.path, p, "dev/a"));
    CHECK_FALSE(readTexture(dir.path, p, "models/x/t"));  // without the slash it is looked up in textures/
    CHECK_FALSE(readTexture(dir.path, p, "/../x"));
}

TEST_CASE("misc_model: surfaces are placed with origin, angles and scale, one triangle each") {
    TempDir dir;
    put(dir.path / "m/q.obj", quadObj("q"));
    put(dir.path / "m/q.png", "x");
    Document d;
    Entity e = modelEntity("m1", "m/q.obj", {10, 0, 0});
    e.keyvalues["model_scale"] = "2";
    d.entities.push_back(e);
    auto s = collectModelSurfaces(d, {}, dir.path);
    REQUIRE(s.size() == 2);
    CHECK(s[0].classname == "misc_model");
    CHECK(s[0].texture == "/m/q");
    CHECK(s[0].castsShadow);
    CHECK(s[0].normal.z == doctest::Approx(1));
    double maxX = 0;
    for (const auto& f : s) for (const Vec3& v : f.verts) maxX = std::max(maxX, v.x);
    CHECK(maxX == doctest::Approx(12));
    // the affine UV map reproduces the file's coordinates: the corner (1,1,0) of the model is uv (1, 0) (v flipped)
    const Vec2 uv = albedoUvAt(s[0], {12, 2, 0});
    CHECK(uv.u == doctest::Approx(1));
    CHECK(uv.v == doctest::Approx(0));

    // yaw 90 turns +X into -Z (Ry), a negative scale mirrors without turning the triangles inside out
    d.entities[0].angles = {0, 90, 0};
    d.entities[0].keyvalues["model_scale"] = "1";
    d.entities[0].keyvalues["_cs"] = "0";
    s = collectModelSurfaces(d, {}, dir.path);
    REQUIRE(s.size() == 2);
    CHECK_FALSE(s[0].castsShadow);
    double minZ = 0;
    for (const auto& f : s) for (const Vec3& v : f.verts) minZ = std::min(minZ, v.z);
    CHECK(minZ == doctest::Approx(-1));
    d.entities[0].angles = {0, 0, 0};
    d.entities[0].keyvalues["model_scale"] = "-1 1 1";
    s = collectModelSurfaces(d, {}, dir.path);
    REQUIRE(s.size() == 2);
    CHECK(s[0].normal.z == doctest::Approx(1));
}

TEST_CASE("misc_model: lightmapped class, occluder unless _cs 0, baked into the world model of the BSP under its own texture name") {
    CHECK(LightingClasses::defaults().isLightmapped("misc_model"));
    CHECK(LightingClasses::defaults().castsShadows("misc_model"));

    TempDir dir;
    put(dir.path / "models/q.obj", quadObj("q"));
    put(dir.path / "models/q.png", "x");
    Document d;
    d.entities.push_back(modelEntity("m1", "models/q.obj", {0, 1, 0}));
    Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(Brush::makeBox("b", {-2, -1, -2}, {2, 0, 2}, "dev/wall"));
    d.entities.push_back(w);

    auto surfaces = collectSurfaces(d, {});
    auto models = collectModelSurfaces(d, {}, dir.path);
    surfaces.insert(surfaces.end(), models.begin(), models.end());
    const OccluderMesh withShadows = buildOccluders(surfaces, LightingClasses::defaults(), nullptr);
    for (auto& s : surfaces) if (s.classname == "misc_model") s.castsShadow = false;
    const OccluderMesh without = buildOccluders(surfaces, LightingClasses::defaults(), nullptr);
    CHECK(withShadows.tris.size() == without.tris.size() + 2 * 9);  // the two triangles of the quad

    BspInput in;
    const DocumentLoader loader = [](const std::string& p) -> const Document& { throw std::runtime_error("no prefab " + p); };
    in.entities = flattenForCompile(d, loader).entities;
    in.surfaces = surfaces;
    in.classes = LightingClasses::defaults();
    const BspOutput out = compileBsp(in);
    BspView v;
    std::string err;
    REQUIRE(v.open(out.bsp, &err));
    std::set<std::string> names;
    for (const auto& s : v.lump<rbsp::Shader>(rbsp::kShaders)) names.insert(s.name);
    CHECK(names.count("models/q") == 1);          // from the game root
    CHECK(names.count("textures/dev/wall") == 1);
    CHECK(v.lump<rbsp::Model>(rbsp::kModels).size() == 1);  // the model is part of the world, not a submodel
    CHECK(v.entities().find("misc_model") == std::string::npos);
}
