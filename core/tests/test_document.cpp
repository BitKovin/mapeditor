#include "core/document.h"
#include "core/entity_class.h"
#include "test_util.h"
#include <doctest.h>

using namespace core;
using namespace testutil;

static bool contains(const std::optional<std::string>& s, const char* needle) {
    return s && s->find(needle) != std::string::npos;
}

TEST_CASE("entity class registry: structural vs convex-required are different sets") {
    for (const char* c : {"worldspawn", "func_detail", "func_group"}) CHECK(EntityClassRegistry::isStructural(c));
    CHECK_FALSE(EntityClassRegistry::isStructural("func_door"));

    CHECK(EntityClassRegistry::requiresConvexBrushes("worldspawn"));
    CHECK(EntityClassRegistry::requiresConvexBrushes("func_group"));
    CHECK_FALSE(EntityClassRegistry::requiresConvexBrushes("func_detail"));  // structural, but free-form
    CHECK_FALSE(EntityClassRegistry::requiresConvexBrushes("func_door"));

    CHECK_FALSE(EntityClassRegistry::showsCustomOriginGizmo("func_detail"));
    CHECK(EntityClassRegistry::showsCustomOriginGizmo("func_door"));
}

TEST_CASE("Entity::showsCustomOriginGizmo needs a brush entity that isn't structural") {
    Entity door;
    door.classname = "func_door";
    door.brushes.push_back(Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}));
    CHECK(door.showsCustomOriginGizmo());

    Entity detail = door;
    detail.classname = "func_detail";
    CHECK_FALSE(detail.showsCustomOriginGizmo());

    Entity light;
    light.classname = "light";
    CHECK_FALSE(light.showsCustomOriginGizmo());
}

TEST_CASE("documentFromJson reads every field") {
    Brush hole = Brush::makeBox("hole", {0, 0, 0}, {1, 1, 1}, "dev/wood");
    hole.op = BrushOp::Subtract;
    hole.faces[0].uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    json door = entityJson("e_door", "func_door", {hole}, {1, 2, 3});
    door["keyvalues"] = {{"speed", "2.5"}};
    json j = docJson({door}, {instanceJson("pi", "prefabs/x.map.json", {4, 5, 6})});

    Document d = documentFromJson(j);
    REQUIRE(d.entities.size() == 1);
    const Entity& e = d.entities[0];
    CHECK(e.classname == "func_door");
    CHECK(e.origin.y == doctest::Approx(2));
    CHECK(e.keyvalues.at("speed") == "2.5");
    REQUIRE(e.brushes.size() == 1);
    CHECK(e.brushes[0].op == BrushOp::Subtract);
    CHECK(e.brushes[0].faces[0].texture == "dev/wood");
    CHECK(e.brushes[0].faces[0].uvs.size() == 4);
    REQUIRE(d.prefabInstances.size() == 1);
    CHECK(d.prefabInstances[0].origin.z == doctest::Approx(6));
    CHECK(d.findEntity("e_door") != nullptr);
    CHECK(d.findEntity("nope") == nullptr);
}

TEST_CASE("documentFromJson reads a face's lightmapScale leniently") {
    auto scaleOf = [](const json& value, bool present) {
        json j = docJson({entityJson("w", "worldspawn", {Brush::makeBox("a", {0, 0, 0}, {1, 1, 1})})});
        if (present) j["entities"][0]["brushes"][0]["faces"][0]["lightmapScale"] = value;
        return documentFromJson(j).entities[0].brushes[0].faces[0].lightmapScale;
    };
    CHECK(scaleOf(nullptr, false) == 1.0);
    CHECK(scaleOf(2.5, true) == doctest::Approx(2.5));
    CHECK(scaleOf(0.0001, true) == doctest::Approx(kMinLightmapScale));
    CHECK(scaleOf(1000, true) == doctest::Approx(kMaxLightmapScale));
    for (const json& bad : {json(-3), json("2"), json(0), json(nullptr)}) CHECK(scaleOf(bad, true) == 1.0);
}

TEST_CASE("documentFromJson is strict about untrusted input") {
    Brush box = Brush::makeBox("b", {0, 0, 0}, {1, 1, 1});
    json good = docJson({entityJson("e", "func_detail", {box})});
    REQUIRE_NOTHROW(documentFromJson(good));

    SUBCASE("wrong schema version") {
        good["schemaVersion"] = "0.1";
        CHECK_THROWS_AS(documentFromJson(good), std::runtime_error);
    }
    SUBCASE("missing schema version") {
        good.erase("schemaVersion");
        CHECK_THROWS(documentFromJson(good));
    }
    SUBCASE("not an object") { CHECK_THROWS(documentFromJson(json::array())); }
    SUBCASE("vertex with 2 components") {
        good["entities"][0]["brushes"][0]["vertices"][0] = json::array({1, 2});
        CHECK_THROWS_AS(documentFromJson(good), std::runtime_error);
    }
    SUBCASE("negative face id") {
        good["entities"][0]["brushes"][0]["faces"][0]["id"] = -1;
        CHECK_THROWS_AS(documentFromJson(good), std::runtime_error);
    }
    SUBCASE("non-string keyvalue") {
        good["entities"][0]["keyvalues"] = {{"speed", 5}};
        CHECK_THROWS_AS(documentFromJson(good), std::runtime_error);
    }
    SUBCASE("unknown brush op") {
        good["entities"][0]["brushes"][0]["op"] = "intersect";
        CHECK_THROWS_AS(documentFromJson(good), std::runtime_error);
    }
}

TEST_CASE("validateDocument") {
    Brush box = Brush::makeBox("b1", {0, 0, 0}, {1, 1, 1});
    auto parse = [](const json& j) { return documentFromJson(j); };

    SUBCASE("a good document passes") {
        CHECK_FALSE(validateDocument(parse(docJson({entityJson("e", "worldspawn", {box})}))).has_value());
    }
    SUBCASE("duplicate entity ids") {
        auto err = validateDocument(parse(docJson({entityJson("e", "func_detail", {}), entityJson("e", "func_detail", {})})));
        CHECK(contains(err, "duplicate entity id"));
    }
    SUBCASE("duplicate brush ids, even across entities") {
        auto err = validateDocument(parse(docJson({entityJson("a", "func_detail", {box}), entityJson("b", "func_detail", {box})})));
        CHECK(contains(err, "duplicate brush id"));
    }
    SUBCASE("worldspawn and func_group must be convex") {
        for (const char* cls : {"worldspawn", "func_group"}) {
            auto err = validateDocument(parse(docJson({entityJson("e", cls, {lPrism("l")})})));
            CHECK(contains(err, "must be convex"));
            CHECK(contains(err, cls));
        }
    }
    SUBCASE("func_detail and dynamic entities may be non-convex") {
        for (const char* cls : {"func_detail", "func_door"})
            CHECK_FALSE(validateDocument(parse(docJson({entityJson("e", cls, {lPrism("l")})}))).has_value());
    }
    SUBCASE("a malformed mesh is rejected for any class") {
        Brush bad = box;
        bad.faces[0].loop[0] = 50;
        CHECK(contains(validateDocument(parse(docJson({entityJson("e", "func_detail", {bad})}))), "references vertex"));
    }
    SUBCASE("prefab instance ids must be unique") {
        auto err = validateDocument(parse(docJson({}, {instanceJson("p", "a.json"), instanceJson("p", "b.json")})));
        CHECK(contains(err, "duplicate prefab instance id"));
    }
}

TEST_CASE("carve scope defaults: self + structural, not other non-structural") {
    Brush hole = Brush::makeBox("h", {0, 0, 0}, {1, 1, 1});
    json bj = brushJson(hole);
    Brush parsed = brushFromJson(bj);
    CHECK(parsed.carve.self);
    CHECK(parsed.carve.structural);
    CHECK_FALSE(parsed.carve.nonStructural);

    bj["carve"] = {{"self", false}, {"nonStructural", true}};
    parsed = brushFromJson(bj);
    CHECK_FALSE(parsed.carve.self);
    CHECK(parsed.carve.structural);  // untouched default
    CHECK(parsed.carve.nonStructural);

    bj["carve"] = {{"self", "yes"}};
    CHECK_THROWS_AS(brushFromJson(bj), std::runtime_error);
}

// ---- N1 step (a): instance names, entity refs, structured overrides (DESIGN.md 18.1a) ----------

TEST_CASE("prefab instance: name, scale, structured overrides and entity refs parse") {
    json inst = instanceJson("pi", "prefabs/door.map.json", {1, 2, 3});
    inst["name"] = "front_door";
    inst["scale"] = {2, 2, 2};
    inst["overrides"] = {{"e_door",
                          {{"keyvalues", {{"speed", "9"}}},
                           {"refs", {{"target", "child/e_btn"}}},
                           {"origin", {1, 0, 0}},
                           {"angles", {0, 90, 0}},
                           {"deleted", false}}},
                         {"child/e_x", {{"deleted", true}}}};
    json ent = entityJson("e_btn", "func_button", {});
    ent["refs"] = {{"target", "e_door"}};
    Document d = documentFromJson(docJson({ent}, {inst}));

    CHECK(d.entities[0].refs.at("target") == "e_door");
    const PrefabInstance& p = d.prefabInstances[0];
    CHECK(p.name == "front_door");
    REQUIRE(p.scale.has_value());
    CHECK(p.scale->x == doctest::Approx(2));
    REQUIRE(p.overrides.size() == 2);
    const EntityOverride& o = p.overrides.at("e_door");
    CHECK(o.keyvalues.at("speed") == "9");
    CHECK(o.refs.at("target") == "child/e_btn");
    REQUIRE(o.origin.has_value());
    CHECK(o.angles->y == doctest::Approx(90));
    CHECK_FALSE(o.deleted);
    CHECK(p.overrides.at("child/e_x").deleted);
    CHECK_FALSE(validateDocument(d).has_value());
}

TEST_CASE("legacy flat string overrides still load as keyvalue overrides") {
    json inst = instanceJson("pi", "prefabs/door.map.json");
    inst["overrides"] = {{"e_door", {{"speed", "3"}, {"keyvalues", "not-a-nested-object"}}}};
    Document d = documentFromJson(docJson({}, {inst}));
    const EntityOverride& o = d.prefabInstances[0].overrides.at("e_door");
    CHECK(o.keyvalues.at("speed") == "3");
    CHECK(o.keyvalues.at("keyvalues") == "not-a-nested-object");
    CHECK_FALSE(o.origin.has_value());
}

TEST_CASE("malformed override / refs / name are rejected") {
    json inst = instanceJson("pi", "prefabs/door.map.json");
    SUBCASE("override that is not an object") {
        inst["overrides"] = {{"e", 5}};
        CHECK_THROWS(documentFromJson(docJson({}, {inst})));
    }
    SUBCASE("bad origin") {
        inst["overrides"] = {{"e", {{"origin", {1, 2}}}}};
        CHECK_THROWS(documentFromJson(docJson({}, {inst})));
    }
    SUBCASE("bad deleted") {
        inst["overrides"] = {{"e", {{"deleted", 1}}}};
        CHECK_THROWS(documentFromJson(docJson({}, {inst})));
    }
    SUBCASE("name pipe is a validation error") {
        inst["name"] = "a|b";
        auto err = validateDocument(documentFromJson(docJson({}, {inst})));
        REQUIRE(err.has_value());
        CHECK(err->find("name") != std::string::npos);
    }
    SUBCASE("duplicate explicit names") {
        json other = instanceJson("pj", "prefabs/other.map.json");
        inst["name"] = "same";
        other["name"] = "same";
        auto err = validateDocument(documentFromJson(docJson({}, {inst, other})));
        REQUIRE(err.has_value());
        CHECK(err->find("same") != std::string::npos);
    }
    SUBCASE("empty path segment in an override key") {
        inst["overrides"] = {{"a//b", {{"deleted", true}}}};
        CHECK(validateDocument(documentFromJson(docJson({}, {inst}))).has_value());
    }
    SUBCASE("entity ref with a pipe") {
        json ent = entityJson("e", "func_button", {});
        ent["refs"] = {{"target", "x|y"}};
        CHECK(validateDocument(documentFromJson(docJson({ent}))).has_value());
    }
}

TEST_CASE("effectiveInstanceNames derives unique default names in document order") {
    json a = instanceJson("a", "prefabs/door_prefab.map.json");
    json b = instanceJson("b", "prefabs/sub/door_prefab.map.json");
    json c = instanceJson("c", "prefabs/door_prefab.map.json");
    json named = instanceJson("n", "prefabs/hinge.map.json");
    named["name"] = "door_prefab3";  // explicit names are reserved: the derived ones skip them
    Document d = documentFromJson(docJson({}, {a, b, c, named}));
    auto names = effectiveInstanceNames(d);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "door_prefab");
    CHECK(names[1] == "door_prefab2");
    CHECK(names[2] == "door_prefab4");
    CHECK(names[3] == "door_prefab3");
    CHECK_FALSE(validateDocument(d).has_value());
}
