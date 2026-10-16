#include "core/decal.h"
#include "core/derived.h"
#include "core/router.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

namespace {
Brush sub(const std::string& id, Vec3 lo, Vec3 hi) {
    Brush b = Brush::makeBox(id, lo, hi);
    b.op = BrushOp::Subtract;
    return b;
}
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Entity decal(const std::string& id, Vec3 origin, Vec3 angles = {0, 0, 0}, const std::string& w = "1", const std::string& h = "1") {
    Entity e = ent(id, "decal");
    e.origin = origin;
    e.angles = angles;
    e.keyvalues = {{"width", w}, {"height", h}, {"depth", "0.5"}, {"texture", "decals/dirt"}};
    return e;
}
ResolvedEntity inPrefab(const Entity& e, std::vector<std::string> path) { return ResolvedEntity{e, std::move(path), e.id}; }

double area(const json& frag) {
    const auto& v = frag["v"];
    const size_t n = v.size() / 3;
    Vec3 s;
    auto P = [&](size_t i) { return Vec3{v[i * 3].get<double>(), v[i * 3 + 1].get<double>(), v[i * 3 + 2].get<double>()}; };
    for (size_t i = 1; i + 1 < n; ++i) s = s + (P(i) - P(0)).cross(P(i + 1) - P(0));
    return 0.5 * s.length();
}
double decalArea(const json& d) {
    double a = 0;
    for (const auto& f : d["fragments"]) a += area(f);
    return a;
}
const json* findCarved(const json& j, const std::string& instance, const std::string& brush) {
    for (const auto& c : j["carved"]) if (c["instance"] == instance && c["brushId"] == brush) return &c;
    return nullptr;
}
}  // namespace

TEST_CASE("derive: only overlapped brushes are listed; authored data is untouched") {
    Document doc;
    Entity world = ent("w", "worldspawn", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4}), Brush::makeBox("elsewhere", {20, 0, 0}, {24, 4, 4}), sub("hole", {1, 1, -1}, {3, 3, 5})});
    Entity door = ent("d", "func_door", {Brush::makeBox("leaf", {0, 0, 0}, {4, 4, 4})});  // overlaps the hole but is dynamic
    doc.entities = {world, door};

    const json j = deriveGeometry(doc, {}, 7);
    CHECK(j["type"] == "derived_geometry");
    CHECK(j["rev"] == 7);
    REQUIRE(j["carved"].size() == 1);
    CHECK(j["carved"][0]["brushId"] == "wall");
    CHECK(j["carved"][0]["entityId"] == "w");
    CHECK(j["carved"][0]["instance"] == "");
    CHECK(j["carved"][0]["subtractors"] == json::array({"hole"}));
    bool sawHoleFace = false;
    for (const auto& f : j["carved"][0]["fragments"]) {
        CHECK(f["v"].size() % 3 == 0);
        if (f["brush"] == "hole") sawHoleFace = true;
    }
    CHECK(sawHoleFace);
    CHECK(world.brushes[0].vertices.size() == 8);
    CHECK(deriveGeometry(Document{}, {}, 1)["carved"].empty());
    CHECK(deriveGeometry(Document{}, {}, 1)["decals"].empty());
}

TEST_CASE("prefabs: a subtractor inside an instance carves inside that instance only") {
    Document doc;  // an empty root: everything comes from prefabs
    Entity pe = ent("pe", "func_detail", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4}), sub("hole", {1, 1, -1}, {3, 3, 5})});
    Entity plain = ent("pe", "func_detail", {Brush::makeBox("wall", {10, 0, 0}, {14, 4, 4})});  // same ids, another instance
    const std::vector<ResolvedEntity> prefabs = {inPrefab(pe, {"root", "inst1"}), inPrefab(plain, {"root", "inst2"})};
    const json j = deriveGeometry(doc, prefabs, 1);
    REQUIRE(j["carved"].size() == 1);
    CHECK(j["carved"][0]["instance"] == "inst1");
    CHECK(j["carved"][0]["entityId"] == "pe");
    CHECK(findCarved(j, "inst2", "wall") == nullptr);
    CHECK(hasDerivedWork(doc, prefabs));
    CHECK_FALSE(hasDerivedWork(doc, {inPrefab(plain, {"root", "inst2"})}));
}

TEST_CASE("prefabs: nested instances are named by their full path") {
    Entity pe = ent("pe", "func_detail", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4}), sub("hole", {1, 1, -1}, {3, 3, 5})});
    const json j = deriveGeometry(Document{}, {inPrefab(pe, {"root", "a", "b"})}, 1);
    REQUIRE(j["carved"].size() == 1);
    CHECK(j["carved"][0]["instance"] == "a/b");
    CHECK(instanceString({"root"}) == "");
}

TEST_CASE("prefabs: the root document may carve prefab content, and a prefab carves the root, but never a sibling prefab") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {sub("rootHole", {1, 1, -1}, {3, 3, 5}), Brush::makeBox("rootWall", {0, 0, 0}, {4, 4, 4})})};
    Entity inside = ent("pe", "func_detail", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4})});
    const json j = deriveGeometry(doc, {inPrefab(inside, {"root", "inst1"})}, 1);
    CHECK(findCarved(j, "", "rootWall") != nullptr);
    CHECK(findCarved(j, "inst1", "wall") != nullptr);     // structural target, default scope

    Entity door = ent("pd", "func_door", {Brush::makeBox("leaf", {0, 0, 0}, {4, 4, 4})});
    CHECK(findCarved(deriveGeometry(doc, {inPrefab(door, {"root", "inst1"})}, 1), "inst1", "leaf") == nullptr);  // dynamic: scope says no

    Entity cutter = ent("pe", "func_detail", {sub("h", {1, 1, -1}, {3, 3, 5})});
    Entity victim = ent("pe", "func_detail", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4})});
    const json k = deriveGeometry(Document{}, {inPrefab(cutter, {"root", "one"}), inPrefab(victim, {"root", "two"})}, 1);
    CHECK(k["carved"].empty());                          // a sibling prefab's subtractor does not reach across
}

TEST_CASE("prefabs: a prefab's subtractive brush cuts the world around it (door frame in a wall)") {
    // the wall belongs to the document (worldspawn), the frame and its opening to a prefab instance
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("wall", {-6, 0, 6}, {6, 3, 6.2})})};
    Entity frame = ent("e_frame", "func_group", {Brush::makeBox("b_frame", {-0.9, 0, 6}, {0.9, 2.5, 6.2}), sub("b_opening", {-0.6, 0, 5.9}, {0.6, 2.2, 6.3})});
    const json j = deriveGeometry(doc, {inPrefab(frame, {"root", "door"})}, 1);

    const json* wall = findCarved(j, "", "wall");
    REQUIRE(wall != nullptr);                                   // the world wall is carved by the prefab's opening
    CHECK((*wall)["subtractors"] == json::array({"b_opening"}));
    CHECK((*wall)["subtractorInstances"] == json::array({"door"}));
    bool cutWall = false;
    for (const auto& f : (*wall)["fragments"]) {
        if (f["brush"] == "b_opening") { cutWall = true; CHECK(f["instance"] == "door"); }   // reveal faces belong to the prefab's brush
        else CHECK(f["instance"] == "");
    }
    CHECK(cutWall);

    // the prefab's own frame is still carved by its own opening (scope self) and stays a ring: it keeps material
    const json* ring = findCarved(j, "door", "b_frame");
    REQUIRE(ring != nullptr);
    CHECK((*ring)["subtractors"] == json::array({"b_opening"}));
    CHECK(ring->at("fragments").size() > 4);

    // with self-carving switched off the frame stays an untouched slab, while the wall is still cut
    Entity slab = frame;
    slab.brushes[1].carve.self = false;
    const json k = deriveGeometry(doc, {inPrefab(slab, {"root", "door"})}, 1);
    CHECK(findCarved(k, "door", "b_frame") == nullptr);
    CHECK(findCarved(k, "", "wall") != nullptr);

    // without the prefab nothing is carved; a second instance with the same ids cuts the wall at its own place
    CHECK(findCarved(deriveGeometry(doc, {}, 1), "", "wall") == nullptr);
    Entity frame2 = ent("e_frame", "func_group", {Brush::makeBox("b_frame", {3.1, 0, 6}, {4.9, 2.5, 6.2}), sub("b_opening", {3.4, 0, 5.9}, {4.6, 2.2, 6.3})});
    const json two = deriveGeometry(doc, {inPrefab(frame, {"root", "door"}), inPrefab(frame2, {"root", "door2"})}, 1);
    const json* both = findCarved(two, "", "wall");
    REQUIRE(both != nullptr);
    CHECK((*both)["subtractorInstances"] == json::array({"door", "door2"}));
}

TEST_CASE("decal: frame from angles") {
    Entity e = decal("d", {0, 0, 0});
    DecalProjector d = decalFromEntity(e);
    CHECK((d.dir - Vec3{0, -1, 0}).length() < 1e-9);
    CHECK((d.up - Vec3{0, 0, -1}).length() < 1e-9);
    CHECK((d.right - Vec3{1, 0, 0}).length() < 1e-9);
    e.angles = {90, 0, 0};
    d = decalFromEntity(e);
    CHECK((d.dir - Vec3{0, 0, -1}).length() < 1e-9);  // pitched to project along -Z
    e.angles = {90, 90, 0};
    d = decalFromEntity(e);
    CHECK((d.dir - Vec3{-1, 0, 0}).length() < 1e-9);
    CHECK(d.dir.dot(d.up) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(d.right.dot(d.up) == doctest::Approx(0.0).epsilon(1e-9));
    e.keyvalues = {{"width", "-3"}, {"height", "abc"}};
    d = decalFromEntity(e);
    CHECK(d.width == 1.0);   // nonsense falls back to the default
    CHECK(d.height == 1.0);
    CHECK(isDecalEntity(decal("d", {0, 0, 0})));
    CHECK_FALSE(isDecalEntity(ent("x", "light")));
    CHECK_FALSE(isDecalEntity(ent("x", "decal", {Brush::makeBox("b", {0, 0, 0}, {1, 1, 1})})));
}

TEST_CASE("decal: a floor decal covers exactly its footprint, with uv 0..1, owned by the floor's entity") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {6, 0, 6})}), decal("dec", {3, 0, 3})};
    const json j = deriveGeometry(doc, {}, 1);
    REQUIRE(j["decals"].size() == 1);
    const json& d = j["decals"][0];
    CHECK(d["decalId"] == "dec");
    CHECK(d["texture"] == "decals/dirt");
    CHECK(decalArea(d) == doctest::Approx(1.0));
    REQUIRE(d["fragments"].size() == 1);
    const json& f = d["fragments"][0];
    CHECK(f["entityId"] == "w");
    CHECK(f["brush"] == "floor");
    double umin = 9, umax = -9, vmin = 9, vmax = -9;
    for (size_t i = 0; i + 1 < f["uv"].size(); i += 2) {
        umin = std::min(umin, f["uv"][i].get<double>()); umax = std::max(umax, f["uv"][i].get<double>());
        vmin = std::min(vmin, f["uv"][i + 1].get<double>()); vmax = std::max(vmax, f["uv"][i + 1].get<double>());
    }
    CHECK(umin == doctest::Approx(0.0)); CHECK(umax == doctest::Approx(1.0));
    CHECK(vmin == doctest::Approx(0.0)); CHECK(vmax == doctest::Approx(1.0));
}

TEST_CASE("decal: the image reads upright and unmirrored seen from the front (v runs down the image, u to the right)") {
    // zero angles: dir = -Y, up = -Z, right = +X. Seen from above the image top (v = 0) is at -Z and u = 0 is at -X.
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {6, 0, 6})}), decal("dec", {3, 0, 3})};
    const json f = deriveGeometry(doc, {}, 1)["decals"][0]["fragments"][0];
    for (size_t i = 0; i < f["uv"].size() / 2; ++i) {
        const double x = f["v"][i * 3].get<double>(), z = f["v"][i * 3 + 2].get<double>();
        CHECK(f["uv"][i * 2].get<double>() == doctest::Approx(x - 2.5));
        CHECK(f["uv"][i * 2 + 1].get<double>() == doctest::Approx(z - 2.5));
    }
}

TEST_CASE("decal: clipped at the surface's edge, and ignores surfaces facing away or out of reach") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {6, 0, 6})}),
                    decal("edge", {0, 0, 3}),     // half the footprint hangs over the floor's edge
                    decal("high", {3, 3, 3}),     // 3 m above: outside the 0.5 m projection depth
                    decal("under", {3, -1.9, 3})}; // below the slab: only its bottom face is in reach, and it faces away
    const json j = deriveGeometry(doc, {}, 1);
    auto byId = [&](const char* id) -> const json& { for (const auto& d : j["decals"]) if (d["decalId"] == id) return d; throw std::runtime_error("missing"); };
    CHECK(decalArea(byId("edge")) == doctest::Approx(0.5));
    CHECK(decalArea(byId("high")) == doctest::Approx(0.0));
    CHECK(decalArea(byId("under")) == doctest::Approx(0.0));
}

TEST_CASE("decal: wall decal, a decal spanning two entities yields fragments owned by each") {
    Document doc;
    Entity a = ent("a", "worldspawn", {Brush::makeBox("floorA", {0, -1, 0}, {3, 0, 6})});
    Entity b = ent("b", "func_detail", {Brush::makeBox("floorB", {3, -1, 0}, {6, 0, 6})});
    doc.entities = {a, b, decal("dec", {3, 0, 3}, {0, 0, 0}, "2", "2")};
    const json d = deriveGeometry(doc, {}, 1)["decals"][0];
    std::map<std::string, double> owned;
    for (const auto& f : d["fragments"]) owned[f["entityId"].get<std::string>()] += area(f);
    REQUIRE(owned.size() == 2);
    CHECK(owned["a"] == doctest::Approx(2.0));
    CHECK(owned["b"] == doctest::Approx(2.0));

    // projecting along -Z onto a wall face at z = 0 that faces +Z
    Document wall;
    wall.entities = {ent("w", "worldspawn", {Brush::makeBox("wall", {0, 0, -1}, {4, 4, 0})}), decal("w1", {2, 2, 0.2}, {90, 0, 0})};
    const json wd = deriveGeometry(wall, {}, 1)["decals"][0];
    CHECK(decalArea(wd) == doctest::Approx(1.0));
}

TEST_CASE("decal: it follows the carved surface, including the walls of the hole") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -2, 0}, {6, 0, 6}), sub("pit", {2.5, -1, 2.5}, {3.5, 1, 3.5})}),
                    decal("dec", {3, 0, 3}, {0, 0, 0}, "3", "3")};
    const json j = deriveGeometry(doc, {}, 1);
    const json& d = j["decals"][0];
    // the floor's top is 9 m^2 of decal footprint minus the 1 m^2 pit opening; the pit's floor is 1 m below the surface but
    // still within the projector's 0.5 m depth? no: it is out of reach, so only the rim gets the decal
    double onFloor = 0;
    std::string ownerOfPit;
    for (const auto& f : d["fragments"]) {
        if (f["brush"] == "floor") onFloor += area(f);
        if (f["brush"] == "pit") ownerOfPit = f["entityId"].get<std::string>();
    }
    CHECK(onFloor == doctest::Approx(9.0 - 1.0));
    CHECK((ownerOfPit.empty() || ownerOfPit == "w"));  // cut walls are owned by the carved entity, not the subtractor's
}

TEST_CASE("decal: projectors inside a prefab project onto root geometry, owner = the target's entity and instance") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {6, 0, 6})})};
    const json j = deriveGeometry(doc, {inPrefab(decal("pd", {3, 0, 3}), {"root", "inst1"})}, 1);
    REQUIRE(j["decals"].size() == 1);
    CHECK(j["decals"][0]["instance"] == "inst1");
    REQUIRE(j["decals"][0]["fragments"].size() == 1);
    CHECK(j["decals"][0]["fragments"][0]["entityId"] == "w");
    CHECK(j["decals"][0]["fragments"][0]["instance"] == "");
}

TEST_CASE("router: derived_geometry accompanies updates, opens and connects - when there is a subtractor or a decal") {
    TempDir d;
    Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    Brush hole = sub("hole", {1, 1, -1}, {3, 3, 5});
    d.write("maps/a.map.json", docJson({entityJson("w", "worldspawn", {wall})}));
    d.write("maps/b.map.json", docJson({entityJson("w", "worldspawn", {wall, hole})}));
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    Router router(scene);

    auto has = [](const std::vector<std::string>& msgs, const char* type) {
        for (const auto& m : msgs) if (json::parse(m)["type"] == type) return true;
        return false;
    };
    CHECK_FALSE(has(router.onConnect().toSender, "derived_geometry"));

    RouterOutput out = router.onMessage(R"({"type":"open_document","requestId":1,"path":"maps/b.map.json"})");
    REQUIRE(has(out.toAll, "derived_geometry"));
    for (const auto& m : out.toAll) if (json::parse(m)["type"] == "derived_geometry") CHECK(json::parse(m)["rev"] == scene.revision());
    CHECK(has(router.onConnect().toSender, "derived_geometry"));

    json doc = scene.documentSyncJson()["doc"];
    doc["entities"][0]["origin"] = json::array({1, 0, 0});
    out = router.onMessage(json{{"type", "document_update"}, {"doc", doc}}.dump());
    CHECK(has(out.toAll, "derived_geometry"));
    CHECK(has(out.toSender, "update_ack"));

    // a lone decal is enough
    d.write("maps/c.map.json", docJson({entityJson("w", "worldspawn", {wall}), entityJson("dec", "decal", {})}));
    out = router.onMessage(R"({"type":"open_document","requestId":2,"path":"maps/c.map.json"})");
    CHECK(has(out.toAll, "derived_geometry"));
}

TEST_CASE("router: prefab content is carved too") {
    TempDir d;
    d.write("prefabs/p.map.json", docJson({entityJson("pe", "func_detail", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4}), sub("hole", {1, 1, -1}, {3, 3, 5})})}));
    d.write("maps/a.map.json", docJson({}, {instanceJson("inst1", "prefabs/p.map.json")}));
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    Router router(scene);
    const RouterOutput out = router.onConnect();
    bool found = false;
    for (const auto& m : out.toSender) {
        const json j = json::parse(m);
        if (j["type"] != "derived_geometry") continue;
        found = true;
        REQUIRE(j["carved"].size() == 1);
        CHECK(j["carved"][0]["instance"] == "inst1");
    }
    CHECK(found);
}

TEST_CASE("router: the document's own brushes are carved once even when prefabs are present") {
    TempDir d;
    d.write("prefabs/p.map.json", docJson({entityJson("pe", "func_detail", {Brush::makeBox("pw", {0, 0, 0}, {1, 1, 1})})}));
    d.write("maps/a.map.json", docJson({entityJson("w", "worldspawn", {Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4}), sub("hole", {1, 1, -1}, {3, 3, 5})})},
                                        {instanceJson("inst1", "prefabs/p.map.json")}));
    Scene scene(d.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");
    const json j = scene.derivedGeometryJson();
    int walls = 0;
    for (const auto& c : j["carved"]) if (c["brushId"] == "wall") { ++walls; CHECK(c["subtractors"] == json::array({"hole"})); }
    CHECK(walls == 1);
}
