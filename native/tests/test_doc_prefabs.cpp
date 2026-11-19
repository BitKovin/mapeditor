// T2.6: prefab files and instance expansion through the Core resolver. The cases of shell/tests/expand.test.ts, converted: the same
// documents and the same expectations, resolved by core::resolvePrefabs (the Shell's expand.ts mirrors it). Plus the cache rules and
// change detection of files on disk. Headless.
#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "base/timing.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/prefabs.h"

namespace fs = std::filesystem;
using namespace native;
using core::Vec3;

namespace {

core::Document docOf(std::vector<core::Entity> entities = {}, std::vector<core::PrefabInstance> instances = {}) {
    core::Document d;
    d.entities = std::move(entities);
    d.prefabInstances = std::move(instances);
    return d;
}

core::PrefabInstance inst(const std::string& id, const std::string& file, Vec3 origin = {0, 0, 0}) {
    core::PrefabInstance p;
    p.id = id;
    p.file = file;
    p.origin = origin;
    return p;
}

core::Entity pt(const std::string& id, Vec3 origin = {0, 0, 0}, const std::string& cls = "info_null") {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.origin = origin;
    return e;
}

core::Entity brushEntity(const std::string& id, const std::string& cls, std::vector<core::Brush> brushes) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

// A document version whose own content is `instances` (and nothing else), with the given path (the cycle check's seed).
std::shared_ptr<const doc::DocumentVersion> versionOf(std::vector<core::PrefabInstance> instances, const std::string& path = "") {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})", path);
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const auto& p : instances) REQUIRE(ed.addInstance(p));
    auto v = ed.finish();
    if (!v) return r.version;
    return v;
}

struct Rig {
    doc::PrefabLibrary lib;
    doc::PrefabResolver resolver{lib};
    std::shared_ptr<const doc::Expansion> expand(std::vector<core::PrefabInstance> instances, const std::string& path = "") {
        return resolver.expand(*versionOf(std::move(instances), path));
    }
};

const core::Entity* byId(const std::vector<core::ResolvedEntity>& r, const std::string& id) {
    for (const auto& e : r)
        if (e.sourceEntityId == id) return &e.entity;
    return nullptr;
}

core::Vec3 boundsMin(const core::Brush& b) {
    Vec3 mn, mx;
    REQUIRE(b.bounds(mn, mx));
    return mn;
}
core::Vec3 boundsMax(const core::Brush& b) {
    Vec3 mn, mx;
    REQUIRE(b.bounds(mn, mx));
    return mx;
}

bool contains(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("prefabs: normalizeAssetPath collapses . and .. and backslashes") {
    CHECK(core::normalizeAssetPath("a/./b/../c") == "a/c");
    CHECK(core::normalizeAssetPath("prefabs\\x.json") == "prefabs/x.json");
    CHECK(core::normalizeAssetPath("../x") == "../x");
}

TEST_CASE("prefabs: an instance offsets the entities and brushes of its file") {
    Rig rig;
    rig.lib.setMemory("c.json", docOf({brushEntity("f", "func_group", {core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1})})}));
    const auto x = rig.expand({inst("pi", "c.json", {10, 20, 30})});
    CHECK(x->problems().empty());
    REQUIRE(x->entityCount() == 1);
    const auto flat = x->flat();
    const core::Vec3 mn = boundsMin(flat[0].entity.brushes[0]);
    CHECK(mn.x == doctest::Approx(9));
    CHECK(mn.y == doctest::Approx(19));
    CHECK(mn.z == doctest::Approx(29));
    CHECK(flat[0].instancePath == std::vector<std::string>{"root", "pi"});
}

TEST_CASE("prefabs: offsets accumulate through three levels and a diamond is fine") {
    Rig rig;
    rig.lib.setMemory("leaf.json", docOf({pt("l", {0, 0, 0}, "light")}));
    rig.lib.setMemory("mid.json", docOf({}, {inst("to_leaf", "leaf.json", {1, 0, 0})}));
    const auto x = rig.expand({inst("a", "mid.json", {10, 0, 0}), inst("b", "mid.json", {20, 0, 0})});
    CHECK(x->problems().empty());
    auto flat = x->flat();
    REQUIRE(flat.size() == 2);
    std::vector<double> xs = {flat[0].entity.origin.x, flat[1].entity.origin.x};
    std::sort(xs.begin(), xs.end());
    CHECK(xs[0] == doctest::Approx(11));
    CHECK(xs[1] == doctest::Approx(21));
    CHECK(flat[0].instancePath == std::vector<std::string>{"root", "a", "to_leaf"});
}

TEST_CASE("prefabs: cycles (also through path aliases), missing files and broken files are reported, nothing throws") {
    Rig rig;
    rig.lib.setMemory("a.json", docOf({}, {inst("to_b", "b.json")}));
    rig.lib.setMemory("b.json", docOf({}, {inst("to_a", "x/../a.json")}));
    CHECK(contains(rig.expand({inst("r", "a.json")})->problems(), "cycle"));
    CHECK(contains(rig.expand({inst("r", "nope.json")})->problems(), "no assets folder"));   // a file that cannot be found (this rig has no folder; "not found" is tested on disk below)
    rig.lib.setMemoryError("e.json", "boom");
    CHECK(contains(rig.expand({inst("r", "e.json")})->problems(), "boom"));

    // one broken instance does not take its siblings down
    rig.lib.setMemory("ok.json", docOf({pt("p")}));
    const auto x = rig.expand({inst("bad", "e.json"), inst("good", "ok.json", {5, 0, 0})});
    CHECK(x->problems().size() == 1);
    REQUIRE(x->instances.size() == 2);
    CHECK(x->instances[0]->entities->empty());
    CHECK(x->instances[1]->entities->size() == 1);
}

TEST_CASE("prefabs: a cycle back to the root document is detected") {
    Rig rig;
    rig.lib.setMemory("p.json", docOf({}, {inst("back", "maps/root.json")}));
    CHECK(contains(rig.expand({inst("p", "p.json")}, "maps/root.json")->problems(), "cycle"));
    // without the root path the same document is just a missing file, not a cycle
    CHECK_FALSE(contains(rig.expand({inst("p", "p.json")})->problems(), "cycle"));
}

TEST_CASE("prefabs: yaw rotates origins about the instance origin and composes entity angles") {
    Rig rig;
    core::Entity p = pt("p", {1, 0, 0});
    p.angles = {0, 45, 0};
    rig.lib.setMemory("c.json", docOf({p}));
    core::PrefabInstance pi = inst("i", "c.json", {10, 0, 0});
    pi.angles = {0, 90, 0};
    const auto flat = rig.expand({pi})->flat();
    const core::Entity* e = byId(flat, "p");
    REQUIRE(e);
    CHECK(e->origin.x == doctest::Approx(10).epsilon(1e-9));
    CHECK(e->origin.z == doctest::Approx(-1).epsilon(1e-9));
    CHECK(e->angles.y == doctest::Approx(135).epsilon(1e-9));
    CHECK(e->angles.x == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("prefabs: nested rotation turns the child offset too") {
    Rig rig;
    rig.lib.setMemory("leaf.json", docOf({pt("p")}));
    rig.lib.setMemory("mid.json", docOf({}, {inst("to_leaf", "leaf.json", {1, 0, 0})}));
    core::PrefabInstance outer = inst("outer", "mid.json", {5, 0, 0});
    outer.angles = {0, 90, 0};
    const core::Entity* e = nullptr;
    const auto flat = rig.expand({outer})->flat();
    e = byId(flat, "p");
    REQUIRE(e);
    CHECK(e->origin.x == doctest::Approx(5).epsilon(1e-9));
    CHECK(e->origin.z == doctest::Approx(-1).epsilon(1e-9));
    CHECK(e->angles.y == doctest::Approx(90).epsilon(1e-9));
}

TEST_CASE("prefabs: scale moves points and brush vertices, sets modelscale_vec on point entities only; a mirror stays outward-wound") {
    Rig rig;
    rig.lib.setMemory("c.json", docOf({pt("p", {2, 0, 0}), brushEntity("b", "func_detail", {core::Brush::makeBox("bb", {1, 1, 1}, {2, 2, 2})})}));
    core::PrefabInstance pi = inst("i", "c.json");
    pi.scale = Vec3{2, 2, 2};
    auto flat = rig.expand({pi})->flat();
    REQUIRE(byId(flat, "p"));
    CHECK(byId(flat, "p")->origin.x == doctest::Approx(4));
    CHECK(byId(flat, "p")->keyvalues.at("modelscale_vec") == "2 2 2");
    const core::Vec3 mn = boundsMin(byId(flat, "b")->brushes[0]);
    const core::Vec3 mx = boundsMax(byId(flat, "b")->brushes[0]);
    CHECK(mn.x == doctest::Approx(2));
    CHECK(mx.z == doctest::Approx(4));
    CHECK(byId(flat, "b")->keyvalues.count("modelscale_vec") == 0);
    pi.scale = Vec3{-1, 1, 1};
    flat = rig.expand({pi})->flat();
    CHECK(byId(flat, "b")->brushes[0].signedVolume() > 0);
}

TEST_CASE("prefabs: overrides of keyvalues, origin, angles, scale and deleted, by nested path, the outer one wins") {
    Rig rig;
    core::Entity a = pt("a", {1, 0, 0});
    a.keyvalues["speed"] = "1";
    rig.lib.setMemory("leaf.json", docOf({a, pt("b")}));
    core::PrefabInstance toLeaf = inst("lf", "leaf.json");
    toLeaf.overrides["a"].keyvalues = {{"speed", "inner"}, {"note", "inner"}};
    rig.lib.setMemory("mid.json", docOf({}, {toLeaf}));
    core::PrefabInstance outer = inst("o", "mid.json");
    core::EntityOverride oa;
    oa.keyvalues = {{"speed", "outer"}};
    oa.origin = Vec3{7, 0, 0};
    oa.angles = Vec3{0, 33, 0};
    oa.scale = Vec3{3, 1, 1};
    outer.overrides["lf/a"] = oa;
    outer.overrides["lf/b"].deleted = true;
    const auto flat = rig.expand({outer})->flat();
    CHECK(byId(flat, "b") == nullptr);
    const core::Entity* e = byId(flat, "a");
    REQUIRE(e);
    CHECK(e->keyvalues.at("speed") == "outer");
    CHECK(e->keyvalues.at("note") == "inner");
    CHECK(e->origin.x == doctest::Approx(7));
    CHECK(e->angles.y == doctest::Approx(33));
    CHECK(e->keyvalues.at("modelscale_vec") == "3 1 1");
}

TEST_CASE("prefabs: a pure translation leaves brush UVs alone") {
    Rig rig;
    core::Brush b = core::Brush::makeBox("bb", {0, 0, 0}, {1, 1, 1});
    b.faces[0].uvs.assign(b.faces[0].loop.size(), core::Vec2{0.25, 0.75});
    rig.lib.setMemory("c.json", docOf({brushEntity("b", "func_detail", {b})}));
    const auto flat = rig.expand({inst("i", "c.json", {5, 0, 0})})->flat();
    const auto& out = flat[0].entity.brushes[0].faces[0].uvs;
    REQUIRE(out.size() == b.faces[0].uvs.size());
    CHECK(out[0].u == 0.25);
    CHECK(out[0].v == 0.75);
}

TEST_CASE("prefabs: decals follow the instance scale") {
    Rig rig;
    core::Entity dec = pt("d", {0, 0, 0}, "decal");
    dec.keyvalues = {{"width", "2"}, {"height", "3"}};
    rig.lib.setMemory("c.json", docOf({dec}));
    core::PrefabInstance pi = inst("i", "c.json");
    pi.scale = Vec3{2, 4, 5};
    auto flat = rig.expand({pi})->flat();
    const core::Entity* e = byId(flat, "d");
    REQUIRE(e);
    CHECK(std::stod(e->keyvalues.at("width")) == doctest::Approx(4));
    CHECK(std::stod(e->keyvalues.at("height")) == doctest::Approx(15));
    CHECK(std::stod(e->keyvalues.at("depth")) == doctest::Approx(2));
    CHECK(e->keyvalues.count("modelscale_vec") == 0);
    core::Entity rotated = dec;
    rotated.angles = {0, 90, 0};
    rig.lib.setMemory("c.json", docOf({rotated}));
    flat = rig.expand({pi})->flat();
    CHECK(std::stod(byId(flat, "d")->keyvalues.at("width")) == doctest::Approx(10));
}

TEST_CASE("prefabs: the cache re-resolves only what changed") {
    Rig rig;
    rig.lib.setMemory("a.json", docOf({pt("pa")}));
    rig.lib.setMemory("b.json", docOf({pt("pb")}));
    const auto v1 = versionOf({inst("ia", "a.json"), inst("ib", "b.json")});
    rig.resolver.expand(*v1);
    CHECK(rig.resolver.resolveCount() == 2);
    rig.resolver.expand(*v1);
    CHECK(rig.resolver.resolveCount() == 2);                      // the same version: all from the cache

    // an unrelated edit (a new entity) shares the instance nodes
    core::Entity extra = pt("extra");
    doc::VersionEditor e1(v1);
    REQUIRE(e1.addEntity(extra));
    const auto v2 = e1.finish();
    rig.resolver.expand(*v2);
    CHECK(rig.resolver.resolveCount() == 2);

    // moving one instance re-resolves that one only
    core::PrefabInstance moved = v2->instances[0]->typed;
    moved.origin = {3, 0, 0};
    doc::VersionEditor e2(v2);
    REQUIRE(e2.replaceInstance(moved));
    const auto v3 = e2.finish();
    const auto x3 = rig.resolver.expand(*v3);
    CHECK(rig.resolver.resolveCount() == 3);
    CHECK(x3->flat()[0].entity.origin.x == doctest::Approx(3));

    // a changed prefab file re-resolves exactly the instances that read it
    core::Entity changed = pt("pb", {0, 9, 0});
    rig.lib.setMemory("b.json", docOf({changed}));
    const auto x4 = rig.resolver.expand(*v3);
    CHECK(rig.resolver.resolveCount() == 4);
    CHECK(x4->instancesUsing({"b.json"}) == std::vector<std::string>{"ib"});
    CHECK(x4->instancesUsing({"a.json"}) == std::vector<std::string>{"ia"});
    CHECK(byId(x4->flat(), "pb")->origin.y == doctest::Approx(9));
    CHECK(x4->instances[0] == x3->instances[0]);                  // the untouched instance is the very same object
}

namespace {
struct TempAssets {
    fs::path root;
    TempAssets() {
        root = fs::temp_directory_path() / ("native_prefabs_" + std::to_string(doc::nextVersionNumber()) + "_" + std::to_string(base::nowMicros()));
        fs::create_directories(root / "prefabs");
    }
    ~TempAssets() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        std::ofstream(root / rel, std::ios::binary | std::ios::trunc) << text;
    }
};
std::string prefabText(const std::string& entityId, double x) {
    return R"({"schemaVersion": "0.2", "entities": [{"id": ")" + entityId + R"(", "classname": "info_null", "origin": [)" + std::to_string(x) + R"(, 0, 0]}]})";
}
} // namespace

TEST_CASE("prefabs: files on disk are loaded, cached and reloaded when they change, vanish or break") {
    TempAssets assets;
    assets.write("prefabs/p.map.json", prefabText("pe", 1));
    doc::PrefabLibrary lib(assets.root);
    doc::PrefabResolver resolver(lib);
    const auto v = versionOf({inst("i", "prefabs/p.map.json", {10, 0, 0})});

    auto x = resolver.expand(*v);
    CHECK(x->problems().empty());
    CHECK(byId(x->flat(), "pe")->origin.x == doctest::Approx(11));
    CHECK(lib.loadCount() == 1);
    resolver.expand(*v);
    CHECK(lib.loadCount() == 1);                                   // cached
    CHECK(lib.refresh().empty());                                  // nothing changed on disk

    const uint64_t v1 = lib.versionOf("prefabs/p.map.json");
    assets.write("prefabs/p.map.json", prefabText("pe", 2.5) + "\n");      // a different size, whatever the clock says
    CHECK(lib.refresh() == std::vector<std::string>{"prefabs/p.map.json"});
    CHECK(lib.versionOf("prefabs/p.map.json") > v1);
    x = resolver.expand(*v);
    CHECK(byId(x->flat(), "pe")->origin.x == doctest::Approx(12.5));
    CHECK(x->instancesUsing({"prefabs/p.map.json"}) == std::vector<std::string>{"i"});

    assets.write("prefabs/p.map.json", "{ not json at all, but long enough to change the size");
    CHECK(lib.refresh().size() == 1);
    x = resolver.expand(*v);
    CHECK(contains(x->problems(), "p.map.json"));
    CHECK(x->entityCount() == 0);
    CHECK(lib.refresh().empty());                                  // a broken file is not re-read until it changes again

    assets.write("prefabs/p.map.json", prefabText("pe", 4) + "\n\n");
    CHECK(lib.refresh().size() == 1);
    CHECK(resolver.expand(*v)->problems().empty());

    fs::remove(assets.root / "prefabs" / "p.map.json");
    CHECK(lib.refresh().size() == 1);
    CHECK(contains(resolver.expand(*v)->problems(), "not found"));
    assets.write("prefabs/p.map.json", prefabText("pe", 6));
    CHECK(lib.refresh().size() == 1);                              // it came back
    CHECK(resolver.expand(*v)->problems().empty());

    // paths that leave the assets folder are refused
    CHECK(contains(resolver.expand(*versionOf({inst("o", "../outside.json")}))->problems(), "outside"));
}
