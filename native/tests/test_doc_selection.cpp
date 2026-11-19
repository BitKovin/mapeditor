// T2.5: the selection model. Converted from shell/tests/store.test.ts (selection survives only while it exists),
// brushEntitySelect.test.ts (a whole brush entity, the sole-brush rule, worldspawn stays a bag of brushes) and instanceActions.test.ts
// (a brush that moved to another entity is followed). Headless.
#include <doctest.h>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"

using namespace native;
using doc::SelRef;

namespace {

std::shared_ptr<const doc::DocumentVersion> emptyDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    return r.version;
}

core::Entity entityWith(const std::string& id, const std::string& cls, std::vector<core::Brush> brushes) {
    core::Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

// world: w1, w2 | door (func_door): d1, d2 | single (func_detail): s1 | light | instance i1
std::shared_ptr<const doc::DocumentVersion> fixture() {
    doc::VersionEditor ed(emptyDoc());
    REQUIRE(ed.addEntity(entityWith("e_world", "worldspawn",
                                    {core::Brush::makeBox("w1", {-3, 0, -3}, {-2, 1, -2}), core::Brush::makeBox("w2", {-6, 0, -3}, {-5, 1, -2})})));
    REQUIRE(ed.addEntity(entityWith("e_door", "func_door",
                                    {core::Brush::makeBox("d1", {2, 0, 2}, {3, 1, 3}), core::Brush::makeBox("d2", {4, 0, 2}, {5, 1, 3})})));
    REQUIRE(ed.addEntity(entityWith("e_single", "func_detail", {core::Brush::makeBox("s1", {8, 0, 8}, {9, 1, 9})})));
    REQUIRE(ed.addEntity(entityWith("e_light", "light_point", {})));
    core::PrefabInstance p;
    p.id = "i1";
    p.file = "prefabs/x.map.json";
    REQUIRE(ed.addInstance(p));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

struct Counter {
    int n = 0;
    void attach(doc::Selection& s) { s.setListener([this] { ++n; }); }
};

} // namespace

TEST_CASE("selection: selecting every brush of a brush entity selects the entity") {
    const auto v = fixture();
    doc::Selection sel;
    sel.selectMany(*v, {SelRef::brush("e_door", "d1"), SelRef::brush("e_door", "d2")});
    REQUIRE(sel.items().size() == 1);
    CHECK(sel.items()[0] == SelRef::entity("e_door"));
    CHECK(*sel.primary() == SelRef::entity("e_door"));
}

TEST_CASE("selection: only some brushes stay brushes") {
    const auto v = fixture();
    doc::Selection sel;
    sel.selectMany(*v, {SelRef::brush("e_door", "d1")});
    CHECK(*sel.primary() == SelRef::brush("e_door", "d1"));
}

TEST_CASE("selection: adding the last brush completes the entity, toggling removes") {
    const auto v = fixture();
    doc::Selection sel;
    sel.select(SelRef::brush("e_door", "d1"));
    sel.toggle(*v, SelRef::brush("e_door", "d2"));
    REQUIRE(sel.items().size() == 1);
    CHECK(sel.items()[0] == SelRef::entity("e_door"));

    sel.toggle(*v, SelRef::brush("e_world", "w1"));
    CHECK(sel.items().size() == 2);
    CHECK(*sel.primary() == SelRef::brush("e_world", "w1"));   // the last added is the primary
    sel.toggle(*v, SelRef::brush("e_world", "w1"));            // again: removed
    CHECK(sel.items().size() == 1);
    CHECK(*sel.primary() == SelRef::entity("e_door"));
    sel.toggle(*v, SelRef::entity("e_door"));
    CHECK(sel.empty());
    CHECK(sel.primary() == nullptr);
}

TEST_CASE("selection: worldspawn brushes stay separate and other selections keep their order") {
    const auto v = fixture();
    const std::vector<SelRef> all = {SelRef::brush("e_world", "w1"), SelRef::brush("e_door", "d1"), SelRef::brush("e_world", "w2"),
                                     SelRef::brush("e_door", "d2")};
    const std::vector<SelRef> want = {SelRef::brush("e_world", "w1"), SelRef::brush("e_world", "w2"), SelRef::entity("e_door")};
    CHECK(doc::wholeEntitiesIfAll(*v, all) == want);
    doc::Selection sel;
    sel.selectMany(*v, {SelRef::brush("e_world", "w1"), SelRef::brush("e_world", "w2")});
    CHECK(sel.items().size() == 2);
    // faces are left alone, even when every face of... every brush is named
    const std::vector<SelRef> faces = {SelRef::face("e_door", "d1", 0), SelRef::face("e_door", "d2", 0)};
    CHECK(doc::wholeEntitiesIfAll(*v, faces) == faces);
}

TEST_CASE("selection: a sole brush IS its entity, and the reverse for mesh editing") {
    const auto v = fixture();
    CHECK(doc::wholeEntityIfSole(*v, SelRef::brush("e_single", "s1")) == SelRef::entity("e_single"));
    CHECK(doc::wholeEntityIfSole(*v, SelRef::brush("e_world", "w1")) == SelRef::brush("e_world", "w1"));         // worldspawn
    CHECK(doc::wholeEntityIfSole(*v, SelRef::brush("e_door", "d1")) == SelRef::brush("e_door", "d1"));           // several brushes
    CHECK(doc::wholeEntityIfSole(*v, SelRef::face("e_single", "s1", 0)) == SelRef::face("e_single", "s1", 0));   // a face stays on its brush
    CHECK(doc::soleBrushOf(*v, SelRef::entity("e_single")) == SelRef::brush("e_single", "s1"));
    CHECK(doc::soleBrushOf(*v, SelRef::entity("e_door")) == SelRef::entity("e_door"));
    CHECK(doc::soleBrushOf(*v, SelRef::entity("e_world")) == SelRef::entity("e_world"));
}

TEST_CASE("selection: edit modes") {
    const auto v = fixture();
    doc::Selection sel;
    // mesh mode needs a brush
    auto r = sel.setMode(*v, doc::EditMode::Mesh);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("Select a brush first") != std::string::npos);
    CHECK(sel.mode() == doc::EditMode::Object);
    sel.select(SelRef::entity("e_light"));
    CHECK_FALSE(sel.setMode(*v, doc::EditMode::Mesh).ok);   // an entity without brushes

    // an entity of one brush is edited through its brush, and goes back to the entity
    sel.select(SelRef::entity("e_single"));
    REQUIRE(sel.setMode(*v, doc::EditMode::Mesh).ok);
    CHECK(sel.mode() == doc::EditMode::Mesh);
    CHECK(*sel.primary() == SelRef::brush("e_single", "s1"));
    sel.setMesh({{0, 1}, {}, {}});
    CHECK(sel.mesh().vertices.size() == 2);
    sel.setMeshElement(doc::MeshElement::Edge);
    CHECK(sel.meshElement() == doc::MeshElement::Edge);
    CHECK(sel.mesh().empty());                                   // the elements of the old kind are dropped
    REQUIRE(sel.setMode(*v, doc::EditMode::Object).ok);
    CHECK(*sel.primary() == SelRef::entity("e_single"));

    // a worldspawn brush stays a brush
    sel.select(SelRef::brush("e_world", "w1"));
    REQUIRE(sel.setMode(*v, doc::EditMode::Mesh).ok);
    REQUIRE(sel.setMode(*v, doc::EditMode::Object).ok);
    CHECK(*sel.primary() == SelRef::brush("e_world", "w1"));

    // leaving face mode drops a face selection to its brush
    REQUIRE(sel.setMode(*v, doc::EditMode::Face).ok);
    sel.select(SelRef::face("e_world", "w1", 2));
    CHECK(sel.primary()->isFace());
    REQUIRE(sel.setMode(*v, doc::EditMode::Object).ok);
    CHECK(*sel.primary() == SelRef::brush("e_world", "w1"));
    CHECK(sel.items().size() == 1);
}

TEST_CASE("selection: select() keeps the mesh selection only for the same brush; listeners hear real changes only") {
    const auto v = fixture();
    doc::Selection sel;
    Counter c;
    c.attach(sel);
    sel.select(SelRef::brush("e_world", "w1"));
    CHECK(c.n == 1);
    const uint64_t rev = sel.revision();
    sel.select(SelRef::brush("e_world", "w1"));   // nothing changes
    CHECK(c.n == 1);
    CHECK(sel.revision() == rev);
    sel.setMesh({{1}, {}, {}});
    CHECK(c.n == 2);
    sel.setMesh({{1}, {}, {}});
    CHECK(c.n == 2);
    sel.select(SelRef::face("e_world", "w1", 0));   // a face of the same brush
    CHECK(sel.mesh().vertices.size() == 1);
    sel.select(SelRef::brush("e_world", "w2"));     // another brush
    CHECK(sel.mesh().empty());
    sel.clear();
    CHECK(sel.empty());
    const int before = c.n;
    sel.clear();
    CHECK(c.n == before);
}

TEST_CASE("selection: it follows the document, keeping only what still exists") {
    const auto v0 = fixture();
    doc::Selection sel;
    sel.selectMany(*v0, {SelRef::brush("e_world", "w1"), SelRef::brush("e_world", "w2"), SelRef::instance("i1")});
    REQUIRE(sel.items().size() == 3);

    {   // the brush moved to a new func_detail entity (a concave result): it is followed there, the others stay
        doc::VersionEditor ed(v0);
        const core::Brush moved = v0->entities[0]->brushes[0]->brush;
        REQUIRE(ed.removeBrush("e_world", "w1"));
        REQUIRE(ed.addEntity(entityWith("e_det", "func_detail", {moved})));
        const auto v1 = ed.finish();
        sel.follow(*v1, false);
        REQUIRE(sel.items().size() == 3);
        CHECK(sel.items()[0] == SelRef::brush("e_det", "w1"));
        CHECK(sel.items()[1] == SelRef::brush("e_world", "w2"));
        CHECK(sel.items()[2] == SelRef::instance("i1"));
    }
    {   // a deleted brush and a deleted instance drop out
        doc::VersionEditor ed(v0);
        REQUIRE(ed.removeBrush("e_world", "w2"));
        REQUIRE(ed.removeInstance("i1"));
        sel.follow(*ed.finish(), false);
        REQUIRE(sel.items().size() == 1);
        CHECK(sel.items()[0] == SelRef::brush("e_world", "w1"));   // this version has it in e_world again: followed back
    }
}

TEST_CASE("selection: a face that is gone, an entity that is gone, mesh mode without its brush") {
    const auto v0 = fixture();
    doc::Selection sel;
    const uint32_t someFace = v0->entities[0]->brushes[0]->brush.faces[0].id;
    sel.select(SelRef::face("e_world", "w1", someFace));
    sel.follow(*v0, false);
    CHECK(sel.items().size() == 1);
    sel.select(SelRef::face("e_world", "w1", 987654));
    sel.follow(*v0, false);
    CHECK(sel.empty());

    sel.select(SelRef::entity("e_door"));
    doc::VersionEditor ed(v0);
    REQUIRE(ed.removeEntity("e_door"));
    sel.follow(*ed.finish(), false);
    CHECK(sel.empty());

    sel.select(SelRef::brush("e_world", "w1"));
    REQUIRE(sel.setMode(*v0, doc::EditMode::Mesh).ok);
    sel.setMesh({{2}, {}, {}});
    sel.follow(*v0, false);                                  // still there: mesh mode and its picks stay
    CHECK(sel.mode() == doc::EditMode::Mesh);
    CHECK(sel.mesh().vertices.size() == 1);
    sel.follow(*v0, true);                                   // undo / redo: the picks are dropped
    CHECK(sel.mesh().empty());
    doc::VersionEditor ed2(v0);
    REQUIRE(ed2.removeBrush("e_world", "w1"));
    sel.follow(*ed2.finish(), false);
    CHECK(sel.empty());
    CHECK(sel.mode() == doc::EditMode::Object);              // the brush is gone: back to object mode
}

TEST_CASE("selection: undo drops a selection whose object disappears (store.test: keeps the selection only while it exists)") {
    const auto v0 = fixture();
    doc::History h(v0);
    doc::Selection sel;
    h.setListener([&](const auto& cur, const doc::ChangeSet&, doc::ChangeCause cause) {
        if (cause == doc::ChangeCause::Reset) sel.reset();
        else sel.follow(*cur, cause == doc::ChangeCause::Undo || cause == doc::ChangeCause::Redo);
    });
    doc::VersionEditor ed(v0);
    REQUIRE(ed.addBrush("e_world", core::Brush::makeBox("w3", {0, 0, 0}, {1, 1, 1})));
    const auto v1 = ed.finish();
    REQUIRE(h.commit({"Add box", v0, v1}));
    sel.select(SelRef::brush("e_world", "w3"));
    CHECK(sel.primary() != nullptr);
    h.undo();
    CHECK(sel.empty());                                       // the box is gone
    h.redo();
    CHECK(sel.empty());                                       // redo does not bring a selection back
    sel.select(SelRef::brush("e_world", "w3"));
    h.reset(v0);                                              // a new document clears everything
    CHECK(sel.empty());
    CHECK(sel.mode() == doc::EditMode::Object);
}
