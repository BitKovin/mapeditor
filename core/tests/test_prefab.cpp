#include "core/prefab.h"
#include <doctest.h>
#include <algorithm>
#include <unordered_map>

using namespace core;

namespace {

Entity makePointEntity(const std::string& id, const std::string& classname) {
    Entity e;
    e.id = id;
    e.classname = classname;
    return e;
}

// Small helper: an in-memory "filesystem" of documents, so these tests don't
// need to touch disk. Mirrors how Scene::loadDocument behaves, just backed
// by a map instead of ifstream.
struct FakeFs {
    std::unordered_map<std::string, Document> docs;

    DocumentLoader loader() {
        return [this](const std::string& path) -> const Document& {
            auto it = docs.find(path);
            if (it == docs.end()) throw std::runtime_error("FakeFs: no such document: " + path);
            return it->second;
        };
    }
};

}  // namespace

TEST_CASE("a root document with no prefab instances resolves to just its own entities") {
    Document root;
    root.entities.push_back(makePointEntity("e_a", "light"));
    root.entities.push_back(makePointEntity("e_b", "light"));

    FakeFs fs;
    auto resolved = resolvePrefabs(root, fs.loader());

    REQUIRE(resolved.size() == 2);
    CHECK(resolved[0].instancePath == std::vector<std::string>{"root"});
    CHECK(resolved[0].sourceEntityId == "e_a");
    CHECK(resolved[1].sourceEntityId == "e_b");
}

TEST_CASE("one level of prefab inclusion offsets the child entity's origin and brushes") {
    Document child;
    Entity e = makePointEntity("e_inner", "light");
    e.origin = Vec3{1, 2, 3};
    child.entities.push_back(e);

    Document root;
    PrefabInstance inst;
    inst.id = "pi_1";
    inst.file = "child.map.json";
    inst.origin = Vec3{100, 0, 0};
    root.prefabInstances.push_back(inst);

    FakeFs fs;
    fs.docs["child.map.json"] = child;

    auto resolved = resolvePrefabs(root, fs.loader());
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].entity.origin.x == doctest::Approx(101));
    CHECK(resolved[0].entity.origin.y == doctest::Approx(2));
    CHECK(resolved[0].entity.origin.z == doctest::Approx(3));
    CHECK(resolved[0].instancePath == std::vector<std::string>{"root", "pi_1"});
    CHECK(resolved[0].sourceEntityId == "e_inner");
}

TEST_CASE("a brush inside a nested prefab is translated by the accumulated offset") {
    Document child;
    Entity e = makePointEntity("e_frame", "func_group");
    e.brushes.push_back(Brush::makeBox("b1", Vec3{-4, -4, -4}, Vec3{4, 4, 4}));
    child.entities.push_back(e);

    Document root;
    PrefabInstance inst;
    inst.id = "pi_1";
    inst.file = "child.map.json";
    inst.origin = Vec3{10, 20, 30};
    root.prefabInstances.push_back(inst);

    FakeFs fs;
    fs.docs["child.map.json"] = child;

    auto resolved = resolvePrefabs(root, fs.loader());
    REQUIRE(resolved.size() == 1);
    Vec3 mn, mx;
    REQUIRE(resolved[0].entity.brushes[0].bounds(mn, mx));
    CHECK(mn.x == doctest::Approx(6));   // -4 + 10
    CHECK(mn.y == doctest::Approx(16));  // -4 + 20
    CHECK(mn.z == doctest::Approx(26));  // -4 + 30
    CHECK(mx.x == doctest::Approx(14));
}

TEST_CASE("the same prefab referenced twice (a diamond) resolves both instances independently") {
    Document leaf;
    leaf.entities.push_back(makePointEntity("e_leaf", "prop_hinge"));

    Document root;
    PrefabInstance a;
    a.id = "pi_a";
    a.file = "leaf.map.json";
    a.origin = Vec3{0, 0, 0};
    PrefabInstance b;
    b.id = "pi_b";
    b.file = "leaf.map.json";
    b.origin = Vec3{50, 0, 0};
    root.prefabInstances = {a, b};

    FakeFs fs;
    fs.docs["leaf.map.json"] = leaf;

    auto resolved = resolvePrefabs(root, fs.loader());
    REQUIRE(resolved.size() == 2);
    CHECK(resolved[0].entity.origin.x == doctest::Approx(0));
    CHECK(resolved[1].entity.origin.x == doctest::Approx(50));
    CHECK(resolved[0].instancePath.back() == "pi_a");
    CHECK(resolved[1].instancePath.back() == "pi_b");
}

TEST_CASE("nesting three levels deep accumulates offsets and instance paths correctly") {
    Document grandchild;
    grandchild.entities.push_back(makePointEntity("e_gc", "prop_hinge"));

    Document child;
    child.entities.push_back(makePointEntity("e_child", "func_group"));
    PrefabInstance toGrandchild;
    toGrandchild.id = "pi_gc";
    toGrandchild.file = "grandchild.map.json";
    toGrandchild.origin = Vec3{1, 0, 0};
    child.prefabInstances.push_back(toGrandchild);

    Document root;
    PrefabInstance toChild;
    toChild.id = "pi_child";
    toChild.file = "child.map.json";
    toChild.origin = Vec3{10, 0, 0};
    root.prefabInstances.push_back(toChild);

    FakeFs fs;
    fs.docs["child.map.json"] = child;
    fs.docs["grandchild.map.json"] = grandchild;

    auto resolved = resolvePrefabs(root, fs.loader());
    REQUIRE(resolved.size() == 2);

    auto byId = [&](const std::string& id) {
        return *std::find_if(resolved.begin(), resolved.end(),
                              [&](const ResolvedEntity& r) { return r.sourceEntityId == id; });
    };
    CHECK(byId("e_child").entity.origin.x == doctest::Approx(10));
    CHECK(byId("e_gc").entity.origin.x == doctest::Approx(11));  // 10 + 1
    CHECK(byId("e_gc").instancePath == std::vector<std::string>{"root", "pi_child", "pi_gc"});
}

TEST_CASE("a direct cycle (A includes B, B includes A) is detected and throws") {
    Document a, b;
    PrefabInstance aToB;
    aToB.id = "pi_b";
    aToB.file = "b.map.json";
    a.prefabInstances.push_back(aToB);

    PrefabInstance bToA;
    bToA.id = "pi_a";
    bToA.file = "a.map.json";
    b.prefabInstances.push_back(bToA);

    FakeFs fs;
    fs.docs["a.map.json"] = a;
    fs.docs["b.map.json"] = b;

    CHECK_THROWS_AS(resolvePrefabs(a, fs.loader(), {}, "a.map.json"), PrefabResolveError);
}

TEST_CASE("a self-referencing prefab is a cycle even at depth zero") {
    Document a;
    PrefabInstance selfRef;
    selfRef.id = "pi_self";
    selfRef.file = "a.map.json";
    a.prefabInstances.push_back(selfRef);

    FakeFs fs;
    fs.docs["a.map.json"] = a;

    CHECK_THROWS_AS(resolvePrefabs(a, fs.loader(), {}, "a.map.json"), PrefabResolveError);
}

TEST_CASE("without a rootPath, a cycle purely among included files is still caught") {
    // root (no path) -> a.map.json -> b.map.json -> a.map.json (cycle)
    Document a, b;
    PrefabInstance aToB;
    aToB.id = "pi_b";
    aToB.file = "b.map.json";
    a.prefabInstances.push_back(aToB);
    PrefabInstance bToA;
    bToA.id = "pi_a_again";
    bToA.file = "a.map.json";
    b.prefabInstances.push_back(bToA);

    Document root;
    PrefabInstance rootToA;
    rootToA.id = "pi_a";
    rootToA.file = "a.map.json";
    root.prefabInstances.push_back(rootToA);

    FakeFs fs;
    fs.docs["a.map.json"] = a;
    fs.docs["b.map.json"] = b;

    CHECK_THROWS_AS(resolvePrefabs(root, fs.loader()), PrefabResolveError);
}

TEST_CASE("a long inclusion chain beyond maxDepth throws rather than recursing forever") {
    // Build a chain root -> f0 -> f1 -> ... -> f9 (10 files), and ask for
    // maxDepth = 5 so resolution must fail partway through.
    constexpr int kChainLength = 10;
    FakeFs fs;
    for (int i = 0; i < kChainLength; ++i) {
        Document d;
        d.entities.push_back(makePointEntity("e_" + std::to_string(i), "light"));
        if (i + 1 < kChainLength) {
            PrefabInstance next;
            next.id = "pi_" + std::to_string(i + 1);
            next.file = "f" + std::to_string(i + 1) + ".map.json";
            d.prefabInstances.push_back(next);
        }
        fs.docs["f" + std::to_string(i) + ".map.json"] = d;
    }

    Document root;
    PrefabInstance toFirst;
    toFirst.id = "pi_f0";
    toFirst.file = "f0.map.json";
    root.prefabInstances.push_back(toFirst);

    ResolveOptions opts;
    opts.maxDepth = 5;
    CHECK_THROWS_AS(resolvePrefabs(root, fs.loader(), opts), PrefabResolveError);
}

// ======================= N1 step (b): angles, scale, overrides, names, refs ==========================
#include "test_util.h"

namespace {
Document docWith(std::vector<Entity> ents, std::vector<PrefabInstance> insts = {}) {
    Document d;
    d.entities = std::move(ents);
    d.prefabInstances = std::move(insts);
    return d;
}
Entity named(const std::string& id, const std::string& name, const std::string& cls = "info_null") {
    Entity e = makePointEntity(id, cls);
    if (!name.empty()) e.keyvalues["targetname"] = name;
    return e;
}
PrefabInstance instOf(const std::string& id, const std::string& file, Vec3 origin = {0, 0, 0}, const std::string& name = "") {
    PrefabInstance p;
    p.id = id;
    p.file = file;
    p.origin = origin;
    p.name = name;
    return p;
}
const FlattenedEntity* findFlat(const FlattenResult& r, const std::string& resolvedName) {
    for (const auto& e : r.entities)
        if (e.resolvedName == resolvedName) return &e;
    return nullptr;
}
const ResolvedEntity* findRes(const std::vector<ResolvedEntity>& r, const std::string& id) {
    for (const auto& e : r)
        if (e.sourceEntityId == id) return &e;
    return nullptr;
}
}  // namespace

TEST_CASE("prefab yaw rotates origins about the instance origin (Ry convention) and composes angles") {
    Entity inner = makePointEntity("p", "info_null");
    inner.origin = {1, 0, 0};
    inner.angles = {0, 45, 0};
    FakeFs fs;
    fs.docs["c.json"] = docWith({inner});
    PrefabInstance pi = instOf("i", "c.json", {10, 0, 0});
    pi.angles = {0, 90, 0};
    auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
    const Entity& e = findRes(r, "p")->entity;
    // Ry(90): (1,0,0) -> (0,0,-1)
    CHECK(e.origin.x == doctest::Approx(10).epsilon(1e-9));
    CHECK(e.origin.z == doctest::Approx(-1).epsilon(1e-9));
    CHECK(e.angles.y == doctest::Approx(135));
    CHECK(e.angles.x == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("angle composition round-trips through the matrix for pitch/yaw/roll") {
    Entity inner = makePointEntity("p", "info_null");
    inner.angles = {20, 30, 40};
    FakeFs fs;
    fs.docs["c.json"] = docWith({inner});
    PrefabInstance pi = instOf("i", "c.json");
    SUBCASE("identity instance keeps the angles") {
        auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
        const Vec3& a = findRes(r, "p")->entity.angles;
        CHECK(a.x == doctest::Approx(20));
        CHECK(a.y == doctest::Approx(30));
        CHECK(a.z == doctest::Approx(40));
    }
    SUBCASE("yaw-only instance adds to yaw") {
        pi.angles = {0, 15, 0};
        auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
        const Vec3& a = findRes(r, "p")->entity.angles;
        CHECK(a.x == doctest::Approx(20));
        CHECK(a.y == doctest::Approx(45));
        CHECK(a.z == doctest::Approx(40));
    }
    SUBCASE("pitch +90 (gimbal lock) still yields an equivalent rotation") {
        Entity probe = makePointEntity("p", "info_null");
        probe.angles = {90, 0, 0};
        FakeFs f2;
        f2.docs["c.json"] = docWith({probe});
        PrefabInstance q = instOf("i", "c.json");
        q.angles = {0, 30, 0};
        auto r = resolvePrefabs(docWith({}, {q}), f2.loader());
        const Vec3& a = findRes(r, "p")->entity.angles;
        CHECK(a.x == doctest::Approx(90).epsilon(1e-6));
        CHECK(a.z == doctest::Approx(0).epsilon(1e-6));
        CHECK(a.y == doctest::Approx(30).epsilon(1e-6));
    }
}

TEST_CASE("nested rotation: the child's offset is rotated by the parent too") {
    Entity leaf = makePointEntity("p", "info_null");
    FakeFs fs;
    fs.docs["leaf.json"] = docWith({leaf});
    fs.docs["mid.json"] = docWith({}, {instOf("to_leaf", "leaf.json", {1, 0, 0})});
    PrefabInstance outer = instOf("outer", "mid.json", {5, 0, 0});
    outer.angles = {0, 90, 0};
    auto r = resolvePrefabs(docWith({}, {outer}), fs.loader());
    const Entity& e = findRes(r, "p")->entity;
    CHECK(e.origin.x == doctest::Approx(5).epsilon(1e-9));
    CHECK(e.origin.z == doctest::Approx(-1).epsilon(1e-9));
    CHECK(e.angles.y == doctest::Approx(90));
}

TEST_CASE("scale: positions, brush vertices and modelscale_vec; a mirror keeps brushes outward-wound") {
    Entity pt = makePointEntity("p", "info_null");
    pt.origin = {2, 0, 0};
    Entity brushy = makePointEntity("b", "func_detail");
    brushy.brushes.push_back(Brush::makeBox("bb", {1, 1, 1}, {2, 2, 2}));
    FakeFs fs;
    fs.docs["c.json"] = docWith({pt, brushy});
    PrefabInstance pi = instOf("i", "c.json");
    pi.scale = Vec3{2, 2, 2};
    auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
    CHECK(findRes(r, "p")->entity.origin.x == doctest::Approx(4));
    CHECK(findRes(r, "p")->entity.keyvalues.at("modelscale_vec") == "2 2 2");
    Vec3 mn, mx;
    REQUIRE(findRes(r, "b")->entity.brushes[0].bounds(mn, mx));
    CHECK(mn.x == doctest::Approx(2));
    CHECK(mx.z == doctest::Approx(4));
    CHECK(findRes(r, "b")->entity.keyvalues.count("modelscale_vec") == 0);

    pi.scale = Vec3{-1, 1, 1};
    r = resolvePrefabs(docWith({}, {pi}), fs.loader());
    const Brush& mirrored = findRes(r, "b")->entity.brushes[0];
    CHECK(mirrored.signedVolume() > 0);
    CHECK_FALSE(mirrored.checkConvexSolid().has_value());
}

TEST_CASE("overrides: keyvalues, origin, angles, scale, deleted; nested path; outer wins over inner") {
    Entity a = named("a", "alpha");
    a.keyvalues["speed"] = "1";
    a.origin = {1, 0, 0};
    Entity b = named("b", "beta");
    FakeFs fs;
    fs.docs["leaf.json"] = docWith({a, b});
    PrefabInstance toLeaf = instOf("lf", "leaf.json");
    EntityOverride innerOv;
    innerOv.keyvalues["speed"] = "inner";
    innerOv.keyvalues["note"] = "inner";
    toLeaf.overrides["a"] = innerOv;  // an override stored inside mid.json
    fs.docs["mid.json"] = docWith({}, {toLeaf});

    PrefabInstance outer = instOf("o", "mid.json");
    EntityOverride outerOv;
    outerOv.keyvalues["speed"] = "outer";
    outerOv.origin = Vec3{7, 0, 0};
    outerOv.angles = Vec3{0, 33, 0};
    outerOv.scale = Vec3{3, 1, 1};
    outer.overrides["lf/a"] = outerOv;
    EntityOverride gone;
    gone.deleted = true;
    outer.overrides["lf/b"] = gone;

    auto r = resolvePrefabs(docWith({}, {outer}), fs.loader());
    REQUIRE(findRes(r, "b") == nullptr);
    const Entity& e = findRes(r, "a")->entity;
    CHECK(e.keyvalues.at("speed") == "outer");  // outer wins
    CHECK(e.keyvalues.at("note") == "inner");   // inner-only keys survive
    CHECK(e.origin.x == doctest::Approx(7));
    CHECK(e.angles.y == doctest::Approx(33));
    CHECK(e.keyvalues.at("modelscale_vec") == "3 1 1");
}

TEST_CASE("legacy-style keyvalue override on a depth-1 entity still applies") {
    Entity a = named("a", "alpha");
    a.keyvalues["speed"] = "1";
    FakeFs fs;
    fs.docs["c.json"] = docWith({a});
    PrefabInstance pi = instOf("i", "c.json");
    pi.overrides["a"].keyvalues["speed"] = "9";
    auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
    CHECK(findRes(r, "a")->entity.keyvalues.at("speed") == "9");
}

TEST_CASE("flatten: resolved names carry the instance names; root entities keep theirs") {
    FakeFs fs;
    fs.docs["door.json"] = docWith({named("d", "door")});
    fs.docs["room.json"] = docWith({named("r", "light")}, {instOf("inner_i", "door.json", {0, 0, 0}, "inner")});
    Document root = docWith({named("w", "world_light")}, {instOf("a", "room.json"), instOf("b", "room.json")});
    auto res = flattenForCompile(root, fs.loader());
    CHECK(res.problems.empty());
    REQUIRE(findFlat(res, "world_light"));
    REQUIRE(findFlat(res, "room|light"));
    REQUIRE(findFlat(res, "room|inner|door"));
    REQUIRE(findFlat(res, "room2|inner|door"));
    CHECK(findFlat(res, "room2|inner|door")->entity.keyvalues.at("targetname") == "room2|inner|door");
    CHECK(findFlat(res, "room2|inner|door")->instancePath == std::vector<std::string>{"root", "b", "inner_i"});
    CHECK(findFlat(res, "room2|inner|door")->instanceNames == std::vector<std::string>{"room2", "inner"});
}

TEST_CASE("flatten refs: inside a prefab, to a child prefab, and across depth") {
    FakeFs fs;
    Entity door = named("door", "door", "func_door");
    fs.docs["door.json"] = docWith({door});
    Entity button = named("btn", "button", "func_button");
    button.refs["target"] = "door_inst/door";           // button -> a child prefab's door
    Entity relay = named("relay", "relay");
    relay.refs["target"] = "btn";                        // relay -> button inside the same prefab
    fs.docs["room.json"] = docWith({button, relay}, {instOf("door_inst", "door.json", {0, 0, 0}, "door_prefab")});
    Entity root_switch = named("sw", "main_switch");
    root_switch.refs["target"] = "r1/btn";               // root entity -> entity in a prefab
    Entity deep = named("deep", "deep_ref");
    deep.refs["target"] = "r1/door_inst/door";           // root entity -> two levels down
    Document root = docWith({root_switch, deep}, {instOf("r1", "room.json", {0, 0, 0}, "room")});
    auto res = flattenForCompile(root, fs.loader());
    CHECK(res.problems.empty());
    CHECK(findFlat(res, "room|button")->entity.keyvalues.at("target") == "room|door_prefab|door");
    CHECK(findFlat(res, "room|relay")->entity.keyvalues.at("target") == "room|button");
    CHECK(findFlat(res, "main_switch")->entity.keyvalues.at("target") == "room|button");
    CHECK(findFlat(res, "deep_ref")->entity.keyvalues.at("target") == "room|door_prefab|door");
    CHECK(findFlat(res, "room|button")->entity.refs.empty());
}

TEST_CASE("flatten refs: sibling child prefabs reference each other via an override in the parent scope") {
    FakeFs fs;
    Entity btn = named("btn", "button", "func_button");
    Entity door = named("door", "door", "func_door");
    fs.docs["btn.json"] = docWith({btn});
    fs.docs["door.json"] = docWith({door});
    PrefabInstance b = instOf("bi", "btn.json", {0, 0, 0}, "b1");
    PrefabInstance d = instOf("di", "door.json", {0, 0, 0}, "d1");
    EntityOverride ov;
    ov.refs["target"] = "di/door";  // declared by instance bi, relative to bi's PARENT scope (the file holding both)
    b.overrides["btn"] = ov;
    fs.docs["room.json"] = docWith({}, {b, d});
    Document root = docWith({}, {instOf("r", "room.json", {0, 0, 0}, "room")});
    auto res = flattenForCompile(root, fs.loader());
    CHECK(res.problems.empty());
    CHECK(findFlat(res, "room|b1|button")->entity.keyvalues.at("target") == "room|d1|door");
}

TEST_CASE("flatten refs: three levels deep, and a child referencing something in its parent's file") {
    FakeFs fs;
    fs.docs["leaf.json"] = docWith({named("lamp", "lamp")});
    Entity ctrl = named("ctrl", "ctrl");
    fs.docs["mid.json"] = docWith({ctrl}, {instOf("lf", "leaf.json", {0, 0, 0}, "leaf")});
    PrefabInstance mid = instOf("m", "mid.json", {0, 0, 0}, "mid");
    EntityOverride ov;
    ov.refs["target"] = "m/lf/lamp";  // an override written in the ROOT scope on mid's entity
    mid.overrides["ctrl"] = ov;
    EntityOverride up;
    up.refs["target"] = "boss";  // leaf's lamp points "up" at an entity of mid's file, declared by instance lf's override
    Document root = docWith({named("boss0", "boss0")}, {mid});
    fs.docs["mid.json"].prefabInstances[0].overrides["lamp"] = up;
    fs.docs["mid.json"].entities.push_back(named("boss", "boss"));
    auto res = flattenForCompile(root, fs.loader());
    CHECK(res.problems.empty());
    CHECK(findFlat(res, "mid|ctrl")->entity.keyvalues.at("target") == "mid|leaf|lamp");
    CHECK(findFlat(res, "mid|leaf|lamp")->entity.keyvalues.at("target") == "mid|boss");
}

TEST_CASE("renaming an instance or entity changes the output names but never breaks a reference") {
    FakeFs fs;
    Entity btn = named("btn", "button");
    btn.refs["target"] = "door";
    fs.docs["room.json"] = docWith({btn, named("door", "door")});
    Document root = docWith({}, {instOf("r", "room.json", {0, 0, 0}, "room")});
    auto before = flattenForCompile(root, fs.loader());
    CHECK(findFlat(before, "room|button")->entity.keyvalues.at("target") == "room|door");

    root.prefabInstances[0].name = "kitchen";
    fs.docs["room.json"].entities[1].keyvalues["targetname"] = "gate";
    auto after = flattenForCompile(root, fs.loader());
    CHECK(after.problems.empty());
    CHECK(findFlat(after, "kitchen|button")->entity.keyvalues.at("target") == "kitchen|gate");
}

TEST_CASE("flatten problems: unresolved ref, deleted target, unnamed target; shared names are fine") {
    FakeFs fs;
    Entity btn = named("btn", "button");
    btn.refs["target"] = "nope";
    btn.refs["killtarget"] = "gone";
    Entity unnamedTarget = named("t", "");
    Entity dupA = named("d1", "same"), dupB = named("d2", "same");
    Entity pointer = named("ptr", "ptr");
    pointer.refs["target"] = "t";
    fs.docs["room.json"] = docWith({btn, named("gone", "gone"), unnamedTarget, dupA, dupB, pointer});
    PrefabInstance pi = instOf("r", "room.json", {0, 0, 0}, "room");
    pi.overrides["gone"].deleted = true;
    auto res = flattenForCompile(docWith({}, {pi}), fs.loader());
    const FlattenedEntity* b = findFlat(res, "room|button");
    REQUIRE(b);
    CHECK(b->entity.keyvalues.count("target") == 0);
    CHECK(b->entity.keyvalues.count("killtarget") == 0);
    CHECK(findFlat(res, "room|ptr")->entity.keyvalues.at("target") == "room|t");  // generated from the entity id
    auto has = [&](const std::string& s) {
        return std::any_of(res.problems.begin(), res.problems.end(), [&](const std::string& p) { return p.find(s) != std::string::npos; });
    };
    CHECK(has("does not resolve"));
    CHECK_FALSE(has("duplicate"));  // several entities may share a name: one reference then triggers them all
    CHECK(has("has no 'targetname'"));
}

TEST_CASE("flatten refs: one key may target several entities, which share one name") {
    FakeFs fs;
    Entity btn = named("btn", "button");
    btn.refs["target"] = RefTargets(std::vector<std::string>{"d1", "d2", "d3", "gone"});
    Entity d1 = named("d1", "door"), d2 = named("d2", "door"), d3 = named("d3", "");  // d3 has no name yet: it joins "door"
    fs.docs["room.json"] = docWith({btn, d1, d2, d3});
    auto res = flattenForCompile(docWith({}, {instOf("r", "room.json", {0, 0, 0}, "room")}), fs.loader());
    CHECK(findFlat(res, "room|button")->entity.keyvalues.at("target") == "room|door");
    int doors = 0;
    for (const auto& e : res.entities) if (e.resolvedName == "room|door") ++doors;
    CHECK(doors == 3);
    auto has = [&](const std::string& s) {
        return std::any_of(res.problems.begin(), res.problems.end(), [&](const std::string& p) { return p.find(s) != std::string::npos; });
    };
    CHECK(has("'gone' does not resolve"));      // the other targets still work
    CHECK(has("takes that name"));
    CHECK_FALSE(has("different names"));
}

TEST_CASE("flatten refs: targets with different names write the first name and say so") {
    FakeFs fs;
    Entity btn = named("btn", "button");
    btn.refs["target"] = RefTargets(std::vector<std::string>{"a", "b"});
    fs.docs["room.json"] = docWith({btn, named("a", "alpha"), named("b", "beta")});
    auto res = flattenForCompile(docWith({}, {instOf("r", "room.json", {0, 0, 0}, "room")}), fs.loader());
    CHECK(findFlat(res, "room|button")->entity.keyvalues.at("target") == "room|alpha");
    CHECK(std::any_of(res.problems.begin(), res.problems.end(), [](const std::string& p) { return p.find("different names") != std::string::npos; }));
}

TEST_CASE("flatten refs: an override replaces all targets of a key") {
    FakeFs fs;
    Entity btn = named("btn", "button");
    btn.refs["target"] = RefTargets(std::vector<std::string>{"a", "b"});
    fs.docs["room.json"] = docWith({btn, named("a", "alpha"), named("b", "alpha"), named("c", "gamma")});
    PrefabInstance pi = instOf("r", "room.json", {0, 0, 0}, "room");
    auto before = flattenForCompile(docWith({}, {pi}), fs.loader());
    CHECK(findFlat(before, "room|button")->entity.keyvalues.at("target") == "room|alpha");
    pi.overrides["btn"].refs["target"] = "r/c";  // relative to the instance's parent scope: instance id, then entity id
    auto after = flattenForCompile(docWith({}, {pi}), fs.loader());
    CHECK(findFlat(after, "room|button")->entity.keyvalues.at("target") == "room|gamma");
}

TEST_CASE("flatten still detects cycles and excess depth; rootPath seeds the cycle check") {
    FakeFs fs;
    fs.docs["a.json"] = docWith({}, {instOf("x", "b.json")});
    fs.docs["b.json"] = docWith({}, {instOf("y", "a.json")});
    CHECK_THROWS_AS(flattenForCompile(docWith({}, {instOf("i", "a.json")}), fs.loader()), PrefabResolveError);
    FakeFs fs2;
    fs2.docs["self.json"] = docWith({named("e", "e")});
    CHECK_THROWS_AS(flattenForCompile(docWith({}, {instOf("i", "self.json")}), fs2.loader(), {}, "self.json"), PrefabResolveError);
    FakeFs deep;
    for (int i = 0; i < 6; ++i) deep.docs["d" + std::to_string(i) + ".json"] = docWith({}, {instOf("n", "d" + std::to_string(i + 1) + ".json")});
    deep.docs["d6.json"] = docWith({named("e", "e")});
    FlattenOptions o;
    o.maxDepth = 4;
    CHECK_THROWS_AS(flattenForCompile(docWith({}, {instOf("i", "d0.json")}), deep.loader(), o), PrefabResolveError);
    o.maxDepth = 16;
    CHECK_NOTHROW(flattenForCompile(docWith({}, {instOf("i", "d0.json")}), deep.loader(), o));
}

TEST_CASE("instance scale stretches a decal's width / height / depth along its own axes, not modelscale_vec") {
    Entity dec = makePointEntity("d", "decal");
    dec.keyvalues["width"] = "2";
    dec.keyvalues["height"] = "3";
    FakeFs fs;
    fs.docs["c.json"] = docWith({dec});
    PrefabInstance pi = instOf("i", "c.json");
    pi.scale = Vec3{2, 4, 5};  // x: width axis, y: depth axis, z: height axis
    auto r = resolvePrefabs(docWith({}, {pi}), fs.loader());
    const Entity& e = findRes(r, "d")->entity;
    CHECK(std::stod(e.keyvalues.at("width")) == doctest::Approx(4));    // 2 * sx
    CHECK(std::stod(e.keyvalues.at("height")) == doctest::Approx(15));  // 3 * sz
    CHECK(std::stod(e.keyvalues.at("depth")) == doctest::Approx(2));    // default 0.5 * sy
    CHECK(e.keyvalues.count("modelscale_vec") == 0);
    // a decal turned 90 degrees about Y inside the prefab swaps which instance axis stretches its width
    FakeFs f2;
    Entity turned = dec;
    turned.angles = {0, 90, 0};
    f2.docs["c.json"] = docWith({turned});
    auto r2 = resolvePrefabs(docWith({}, {pi}), f2.loader());
    CHECK(std::stod(findRes(r2, "d")->entity.keyvalues.at("width")) == doctest::Approx(10));  // 2 * sz
}
