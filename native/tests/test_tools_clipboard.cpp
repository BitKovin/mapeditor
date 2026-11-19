// T4.3: the document edits behind Delete, Duplicate, Group / Ungroup and Copy / Paste (tools/edit_ops); converted from shell/tests/clipboard.test.ts.
#include <doctest.h>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/edit_ops.h"

using namespace native;
using doc::SelRef;

namespace {

core::Entity worldWithBox() {
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    return w;
}

std::shared_ptr<const doc::DocumentVersion> docOf(const std::vector<core::Entity>& entities, const std::vector<core::PrefabInstance>& instances = {}) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    for (const core::Entity& e : entities) REQUIRE(ed.addEntity(e));
    for (const core::PrefabInstance& p : instances) REQUIRE(ed.addInstance(p));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

} // namespace

TEST_CASE("clipboard: a brush round-trips through JSON and pastes with fresh ids, offset") {
    const auto base = docOf({worldWithBox()});
    const auto payload = tools::buildClipboard(*base, {SelRef::brush("w", "b")});
    REQUIRE(payload);
    const auto back = tools::parseClipboard(tools::serializeClipboard(*payload));
    REQUIRE(back);
    const tools::EditResult r = tools::pasteInto(base, *back, {4, 0, 0});
    REQUIRE(r.ok());
    const auto& brushes = r.version->entities[0]->brushes;
    REQUIRE(brushes.size() == 2);
    CHECK(brushes[1]->brush.id != brushes[0]->brush.id);
    CHECK(brushes[1]->brush.vertices[0].x == doctest::Approx(brushes[0]->brush.vertices[0].x + 4));
    REQUIRE(r.select.size() == 1);
    CHECK(r.select[0] == SelRef::brush("w", brushes[1]->brush.id));
}

TEST_CASE("clipboard: whole entities and prefab instances are copied; pasting makes new ones") {
    core::Entity light;
    light.id = "e1";
    light.classname = "light";
    light.origin = {1, 2, 3};
    core::PrefabInstance inst;
    inst.id = "p1";
    inst.file = "p/door.map.json";
    const auto base = docOf({light}, {inst});
    const auto payload = tools::buildClipboard(*base, {SelRef::entity("e1"), SelRef::instance("p1")});
    REQUIRE(payload);
    const tools::EditResult r = tools::pasteInto(base, *payload, {1, 0, 0});
    REQUIRE(r.ok());
    REQUIRE(r.version->entities.size() == 2);
    CHECK(r.version->entities[1]->typed.origin.x == doctest::Approx(2));
    CHECK(r.version->entities[1]->typed.origin.y == doctest::Approx(2));
    REQUIRE(r.version->instances.size() == 2);
    CHECK(r.version->instances[0]->typed.name.empty());
    CHECK(r.version->instances[1]->typed.name == "door2");
}

TEST_CASE("clipboard: text that is not editor clipboard JSON is rejected") {
    CHECK_FALSE(tools::parseClipboard("hello"));
    CHECK_FALSE(tools::parseClipboard("{\"a\":1}"));
    CHECK_FALSE(tools::parseClipboard(R"({"format": "level-editor/clipboard", "version": 99, "items": []})"));
    CHECK_FALSE(tools::parseClipboard(R"({"format": "level-editor/clipboard", "version": 1, "items": []})"));   // nothing usable in it
}

TEST_CASE("clipboard: a brush of a brush entity goes back into an entity of the same class, else into the world") {
    core::Entity detail;
    detail.id = "d";
    detail.classname = "func_detail";
    detail.brushes.push_back(core::Brush::makeBox("db", {3, 3, 3}, {4, 4, 4}, "dev/grey"));
    const auto base = docOf({worldWithBox(), detail});
    const auto payload = tools::buildClipboard(*base, {SelRef::brush("d", "db")});
    REQUIRE(payload);
    const tools::EditResult r = tools::pasteInto(base, *payload);
    REQUIRE(r.ok());
    CHECK(r.version->entities[1]->brushes.size() == 2);
    // the source entity is gone: the brush lands in the world
    const auto without = docOf({worldWithBox()});
    const tools::EditResult r2 = tools::pasteInto(without, *payload);
    REQUIRE(r2.ok());
    CHECK(r2.version->entities[0]->brushes.size() == 2);
}

TEST_CASE("edit ops: delete, duplicate, group and ungroup") {
    core::Entity detail;
    detail.id = "d";
    detail.classname = "func_detail";
    detail.brushes.push_back(core::Brush::makeBox("db", {3, 3, 3}, {4, 4, 4}, "dev/grey"));
    core::Entity light;
    light.id = "l";
    light.classname = "light";
    const auto base = docOf({worldWithBox(), detail, light});

    SUBCASE("delete removes brushes, entities and instances") {
        const tools::EditResult r = tools::deleteSelection(base, {SelRef::brush("w", "b"), SelRef::entity("l")});
        REQUIRE(r.ok());
        CHECK(r.version->entities[0]->brushes.empty());
        CHECK(r.version->findEntity("l") < 0);
        CHECK(r.version->findEntity("d") >= 0);
    }
    SUBCASE("a brush whose entity is selected goes with the entity") {
        const tools::EditResult r = tools::deleteSelection(base, {SelRef::entity("d"), SelRef::brush("d", "db")});
        REQUIRE(r.ok());
        CHECK(r.version->findEntity("d") < 0);
    }
    SUBCASE("duplicate copies brushes into their entity and whole entities as new ones, selecting the copies") {
        const tools::EditResult r = tools::duplicateSelection(base, {SelRef::brush("w", "b"), SelRef::entity("l")});
        REQUIRE(r.ok());
        CHECK(r.version->entities[0]->brushes.size() == 2);
        CHECK(r.version->entities.size() == 4);
        CHECK(r.select.size() == 2);
        CHECK(r.label == "Duplicate 2 items");
    }
    SUBCASE("nothing selected: nothing to do") {
        CHECK_FALSE(tools::deleteSelection(base, {}).ok());
        CHECK_FALSE(tools::duplicateSelection(base, {}).ok());
    }
    SUBCASE("group moves brushes into a new func_group; ungroup brings them back") {
        const tools::EditResult g = tools::makeEntityFromSelection(base, {SelRef::brush("w", "b")}, "func_group");
        REQUIRE(g.ok());
        CHECK(g.version->entities[0]->brushes.empty());
        REQUIRE(g.version->entities.size() == 4);
        const auto& grp = g.version->entities.back();
        CHECK(grp->typed.classname == "func_group");
        CHECK(grp->brushes.size() == 1);
        REQUIRE(g.select.size() == 1);
        const tools::EditResult u = tools::moveSelectionToWorld(g.version, {SelRef::entity(grp->typed.id)});
        REQUIRE(u.ok());
        CHECK(u.version->entities[0]->brushes.size() == 1);
    }
    SUBCASE("move to world refuses brushes that are already in the world") {
        const tools::EditResult u = tools::moveSelectionToWorld(base, {SelRef::brush("w", "b")});
        CHECK_FALSE(u.ok());
        CHECK_FALSE(u.error.empty());
    }
}

TEST_CASE("edit ops: unknown keys of entities survive a paste") {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": [
      {"id": "w", "classname": "worldspawn", "futureKey": 7, "brushes": []}]})");
    REQUIRE(r.version);
    core::Entity l;
    l.id = "l";
    l.classname = "light";
    doc::VersionEditor ed(r.version);
    REQUIRE(ed.addEntity(l, doc::Json{{"id", "l"}, {"classname", "light"}, {"extra", "keep"}}));
    const auto v = ed.finish();
    REQUIRE(v);
    const auto payload = tools::buildClipboard(*v, {SelRef::entity("l")});
    REQUIRE(payload);
    const tools::EditResult p = tools::pasteInto(v, *payload);
    REQUIRE(p.ok());
    CHECK(p.version->entities.back()->raw.value("extra", "") == "keep");
}
