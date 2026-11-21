// T11.7a: turn into prefab (tools/prefabize), converted from shell/tests/prefabize.test.ts.
#include <doctest.h>

#include <algorithm>
#include <cmath>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/parametric.h"
#include "tools/prefabize.h"

using namespace native;
using Version = std::shared_ptr<const doc::DocumentVersion>;
using doc::SelRef;

namespace {

core::Entity light(const std::string& id, core::Vec3 origin) {
    core::Entity e;
    e.id = id;
    e.classname = "light";
    e.origin = origin;
    return e;
}

// worldspawn "w" with the boxes a (x 10..12) and b (x 14..16), the lights l1 (12, 5, 0) -> l2, "gone", and l2 at the origin.
Version level() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    REQUIRE(ed.addEntity(w));
    REQUIRE(ed.addBrush("w", core::Brush::makeBox("a", {10, 0, 0}, {12, 2, 2})));
    REQUIRE(ed.addBrush("w", core::Brush::makeBox("b", {14, 0, 0}, {16, 2, 2})));
    core::Entity l1 = light("l1", {12, 5, 0});
    l1.refs["target"] = core::RefTargets(std::vector<std::string>{"l2", "gone"});
    REQUIRE(ed.addEntity(l1));
    REQUIRE(ed.addEntity(light("l2", {0, 0, 0})));
    const Version v = ed.finish();
    REQUIRE(v);
    return v;
}

const doc::Json& entityOf(const doc::Json& file, const std::string& id) {
    for (const doc::Json& e : file["entities"])
        if (e["id"] == id) return e;
    FAIL("no entity " << id);
    return file;
}

} // namespace

TEST_CASE("prefabize: the prefab is centred on the selection pivot and leaves other brushes out") {
    const Version v = level();
    const auto ex = tools::extractPrefab(*v, {SelRef::brush("w", "a"), SelRef::entity("l1")});
    REQUIRE(ex);
    CHECK(ex->pivot[0] > 10);
    CHECK(ex->pivot[0] < 12.5);
    const doc::Json& world = entityOf(ex->file, "w");
    REQUIRE(world["brushes"].size() == 1);
    double lo = 1e9, hi = -1e9;
    for (const doc::Json& p : world["brushes"][0]["vertices"]) {
        lo = std::min(lo, p[0].get<double>());
        hi = std::max(hi, p[0].get<double>());
    }
    CHECK((lo + hi) / 2 == doctest::Approx(11 - ex->pivot[0]));
    const doc::Json& l = entityOf(ex->file, "l1");
    CHECK(l["origin"][0].get<double>() == doctest::Approx(std::round((12 - ex->pivot[0]) * 1000) / 1000));
    CHECK(l["origin"][1].get<double>() == doctest::Approx(std::round((5 - ex->pivot[1]) * 1000) / 1000));
    CHECK_FALSE(l.contains("refs"));   // the targets left behind are dropped
    CHECK(ex->pivot[0] == doctest::Approx(std::round(ex->pivot[0] * 1000) / 1000));
}

TEST_CASE("prefabize: references with several targets keep the ones inside the selection") {
    Version v = level();
    {
        doc::VersionEditor ed(v);
        core::Entity l1 = v->entities[static_cast<size_t>(v->findEntity("l1"))]->typed;
        l1.refs["target"] = core::RefTargets(std::vector<std::string>{"l2", "l3", "gone"});
        REQUIRE(ed.replaceEntity(l1));
        REQUIRE(ed.addEntity(light("l3", {1, 1, 1})));
        v = ed.finish();
        REQUIRE(v);
    }
    const auto all = tools::extractPrefab(*v, {SelRef::entity("l1"), SelRef::entity("l2"), SelRef::entity("l3")});
    REQUIRE(all);
    CHECK(entityOf(all->file, "l1")["refs"]["target"] == doc::Json::array({"l2", "l3"}));
    const auto one = tools::extractPrefab(*v, {SelRef::entity("l1"), SelRef::entity("l3")});
    REQUIRE(one);
    CHECK(entityOf(one->file, "l1")["refs"]["target"] == "l3");
}

TEST_CASE("prefabize: references between selected entities stay") {
    const Version v = level();
    const auto ex = tools::extractPrefab(*v, {SelRef::entity("l1"), SelRef::entity("l2")});
    REQUIRE(ex);
    CHECK(entityOf(ex->file, "l1")["refs"]["target"] == "l2");
}

TEST_CASE("prefabize: the selection is swapped for one instance at the pivot, the other brush stays") {
    const Version v = level();
    const std::vector<SelRef> sel{SelRef::brush("w", "a")};
    const auto ex = tools::extractPrefab(*v, sel);
    REQUIRE(ex);
    const tools::EditResult r = tools::replaceWithInstance(v, sel, "prefabs/crate.map.json", ex->pivot);
    INFO(r.error);
    REQUIRE(r.version);
    const auto& w = r.version->entities[static_cast<size_t>(r.version->findEntity("w"))];
    REQUIRE(w->brushes.size() == 1);
    CHECK(w->brushes[0]->brush.id == "b");
    REQUIRE(r.version->instances.size() == 1);
    const core::PrefabInstance& p = r.version->instances[0]->typed;
    CHECK(p.file == "prefabs/crate.map.json");
    CHECK(p.name == "crate");
    CHECK(p.origin.x == doctest::Approx(ex->pivot[0]));
    CHECK(p.origin.y == doctest::Approx(ex->pivot[1]));
    CHECK(p.origin.z == doctest::Approx(ex->pivot[2]));
    CHECK(p.angles.x == 0);
    CHECK(p.angles.y == 0);
    REQUIRE(r.select.size() == 1);
    CHECK(r.select[0] == SelRef::instance(p.id));
    CHECK(r.label == "Turn into prefab");
}

TEST_CASE("prefabize: a whole selected entity is removed, the world only loses its brushes") {
    const Version v = level();
    const std::vector<SelRef> sel{SelRef::entity("w"), SelRef::entity("l1")};
    const auto ex = tools::extractPrefab(*v, sel);
    REQUIRE(ex);
    const tools::EditResult r = tools::replaceWithInstance(v, sel, "prefabs/all.map.json", ex->pivot);
    REQUIRE(r.version);
    CHECK(r.version->findEntity("l1") < 0);
    CHECK(r.version->findEntity("l2") >= 0);
    const int w = r.version->findEntity("w");
    REQUIRE(w >= 0);
    CHECK(r.version->entities[static_cast<size_t>(w)]->brushes.empty());
    CHECK(r.version->instances.size() == 1);
}

TEST_CASE("prefabize: selected prefab instances are carried into the prefab, relative to the pivot") {
    const doc::LoadResult r0 = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": [], "prefabInstances": [{"id": "p1", "file": "p/door.map.json", "origin": [4, 0, 0]}]})");
    REQUIRE(r0.version);
    const std::vector<SelRef> sel{SelRef::instance("p1")};
    const auto ex = tools::extractPrefab(*r0.version, sel);
    REQUIRE(ex);
    CHECK(ex->pivot[0] == doctest::Approx(4));
    REQUIRE(ex->file["prefabInstances"].size() == 1);
    CHECK(ex->file["prefabInstances"][0]["file"] == "p/door.map.json");
    CHECK(ex->file["prefabInstances"][0]["origin"][0].get<double>() == doctest::Approx(0));
    const tools::EditResult r = tools::replaceWithInstance(r0.version, sel, "p/new.map.json", ex->pivot);
    REQUIRE(r.version);
    REQUIRE(r.version->instances.size() == 1);
    CHECK(r.version->instances[0]->typed.file == "p/new.map.json");
}

TEST_CASE("prefabize: nothing selected, nothing to extract") {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    CHECK_FALSE(tools::extractPrefab(*r.version, {}));
    CHECK_FALSE(tools::extractPrefab(*r.version, {SelRef::entity("nope")}));
}

TEST_CASE("prefabize: the extracted file loads back as a document with the selection in it") {
    const Version v = level();
    const auto ex = tools::extractPrefab(*v, {SelRef::brush("w", "a"), SelRef::entity("l1"), SelRef::entity("l2")});
    REQUIRE(ex);
    const doc::LoadResult back = doc::loadDocumentText(ex->file.dump());
    INFO(back.error);
    REQUIRE(back.version);
    CHECK(back.version->findEntity("l2") >= 0);
    CHECK(back.version->findBrush("w", "a") >= 0);
    CHECK(back.version->findBrush("w", "b") < 0);
}

TEST_CASE("prefabize: file names") {
    std::string err;
    CHECK(tools::prefabFileName("prefabs", "crate", err) == "prefabs/crate.map.json");
    CHECK(err.empty());
    CHECK(tools::prefabFileName("prefabs", "props\\crate", err) == "prefabs/props/crate.map.json");
    CHECK(tools::prefabFileName("prefabs", "  crate.map.json ", err) == "prefabs/crate.map.json");
    CHECK(tools::prefabFileName("prefabs", "prefabs/crate", err) == "prefabs/crate.map.json");
    CHECK(tools::prefabFileName("prefabs/", "x.json", err) == "prefabs/x.json");
    CHECK(tools::prefabFileName("prefabs", "", err).empty());
    CHECK_FALSE(err.empty());
    CHECK(tools::prefabFileName("prefabs", "../evil", err).empty());
    CHECK(tools::prefabFileName("prefabs", "C:/evil", err).empty());
    CHECK(tools::prefabFileName("prefabs", "/evil", err).empty());
    CHECK(tools::prefabFileName("prefabs", "a?b", err).empty());
    CHECK(tools::prefabFileName("prefabs", "a//b", err).empty());
}
