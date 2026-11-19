// T2.4: version editor, change sets and the undo history. Headless, no GPU.
#include <doctest.h>

#include <algorithm>
#include <filesystem>

#include "base/timing.h"
#include "doc/changeset.h"
#include "doc/commands.h"
#include "doc/document.h"
#include "doc/edit.h"
#include "doc/io.h"

namespace fs = std::filesystem;
using namespace native;

namespace {

const fs::path kLvl2 = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData/maps/lvl2.map.json";

std::shared_ptr<const doc::DocumentVersion> loadDemo() {
    const doc::LoadResult r = doc::loadDocumentFile(fs::path(NATIVE_CORE_ASSETS_DIR) / "maps" / "demo.map.json", "maps/demo.map.json");
    INFO(r.error);
    REQUIRE(r.version);
    return r.version;
}

// The entity with the most brushes (the world).
size_t worldIndex(const doc::DocumentVersion& v) {
    size_t best = 0;
    for (size_t i = 0; i < v.entities.size(); ++i)
        if (v.entities[i]->brushes.size() > v.entities[best]->brushes.size()) best = i;
    return best;
}

// A new version with one brush replaced by edit(brush).
template <class F>
std::shared_ptr<const doc::DocumentVersion> editBrush(const std::shared_ptr<const doc::DocumentVersion>& v, size_t entity, size_t brush, F edit) {
    core::Brush b = v->entities[entity]->brushes[brush]->brush;
    edit(b);
    doc::VersionEditor ed(v);
    REQUIRE(ed.replaceBrush(v->entities[entity]->typed.id, b));
    auto next = ed.finish();
    REQUIRE(next);
    return next;
}

const doc::ChangedEntity* findEntity(const doc::ChangeSet& c, const std::string& id) {
    for (const auto& e : c.entities)
        if (e.entityId == id && e.instance.empty()) return &e;
    return nullptr;
}

} // namespace

TEST_CASE("diff: identical versions differ by nothing") {
    const auto v = loadDemo();
    CHECK(doc::diff(*v, *v).empty());
    // a copy with a new number (as after Save As) shares every node
    auto copy = std::make_shared<doc::DocumentVersion>(*v);
    copy->version = doc::nextVersionNumber();
    const doc::ChangeSet c = doc::diff(*v, *copy);
    CHECK(c.empty());
    CHECK(c.fromVersion == v->version);
    CHECK(c.toVersion == copy->version);
    // an editor that changed nothing hands back the base
    doc::VersionEditor ed(v);
    CHECK(ed.finish() == v);
}

TEST_CASE("diff: one moved brush is one ChangedBrush with both boxes") {
    const auto v = loadDemo();
    const size_t w = worldIndex(*v);
    REQUIRE(!v->entities[w]->brushes.empty());
    const core::Brush& old = v->entities[w]->brushes[0]->brush;
    const auto next = editBrush(v, w, 0, [](core::Brush& b) { b.translate({10, 0, 0}); });
    CHECK(next->version > v->version);
    CHECK(next->entities[w] != v->entities[w]);                       // the entity node was copied
    CHECK(next->entities[w]->brushes[0] != v->entities[w]->brushes[0]);
    if (v->entities[w]->brushes.size() > 1) CHECK(next->entities[w]->brushes[1] == v->entities[w]->brushes[1]);   // the others are shared

    const doc::ChangeSet c = doc::diff(*v, *next);
    CHECK_FALSE(c.all);
    CHECK(c.entities.empty());
    REQUIRE(c.brushes.size() == 1);
    const doc::ChangedBrush& cb = c.brushes[0];
    CHECK(cb.entityId == v->entities[w]->typed.id);
    CHECK(cb.brushId == old.id);
    CHECK((cb.kinds & doc::Geometry) != 0);
    REQUIRE(cb.before.valid());
    REQUIRE(cb.after.valid());
    const doc::Box ob = doc::brushBox(old);
    CHECK(cb.before == ob);
    CHECK(cb.after.mn.x == doctest::Approx(ob.mn.x + 10));
    CHECK(cb.after.mx.x == doctest::Approx(ob.mx.x + 10));
    CHECK(cb.after.mn.y == doctest::Approx(ob.mn.y));
}

TEST_CASE("diff: a texture change is a Surface change, not Geometry") {
    const auto v = loadDemo();
    const size_t w = worldIndex(*v);
    const auto next = editBrush(v, w, 0, [](core::Brush& b) { b.faces[0].texture = "base_wall/concrete01"; });
    const doc::ChangeSet c = doc::diff(*v, *next);
    REQUIRE(c.brushes.size() == 1);
    CHECK(c.brushes[0].kinds == doc::Surface);
    CHECK(c.brushes[0].before == c.brushes[0].after);
}

TEST_CASE("diff: entities added, removed, keys, transform and class") {
    const auto v = loadDemo();
    core::Entity light;
    light.id = "e_test_light";
    light.classname = "light_point";
    light.keyvalues["intensity"] = "3";
    light.origin = {1, 2, 3};
    doc::VersionEditor add(v);
    REQUIRE(add.addEntity(light));
    const auto withLight = add.finish();
    REQUIRE(withLight);

    doc::ChangeSet c = doc::diff(*v, *withLight);
    REQUIRE(c.entities.size() == 1);
    CHECK(c.entities[0].entityId == "e_test_light");
    CHECK((c.entities[0].kinds & doc::Light) != 0);
    CHECK_FALSE(c.entities[0].before.valid());
    CHECK(c.entities[0].after.valid());
    CHECK(doc::diff(*withLight, *v).entities[0].before.valid());      // and back: removed

    core::Entity edited = withLight->entities.back()->typed;
    edited.keyvalues["intensity"] = "5";
    doc::VersionEditor e1(withLight);
    REQUIRE(e1.replaceEntity(edited));
    c = doc::diff(*withLight, *e1.finish());
    REQUIRE(c.entities.size() == 1);
    CHECK(c.entities[0].kinds == (doc::Keys | doc::Light));
    CHECK(c.brushes.empty());

    edited.origin = {4, 2, 3};
    doc::VersionEditor e2(withLight);
    REQUIRE(e2.replaceEntity(edited));
    c = doc::diff(*withLight, *e2.finish());
    REQUIRE(c.entities.size() == 1);
    CHECK((c.entities[0].kinds & doc::Transform) != 0);
    CHECK(c.entities[0].before.mn.x == 1);
    CHECK(c.entities[0].after.mn.x == 4);

    core::Entity renamed = withLight->entities.back()->typed;
    renamed.classname = "point_entity";
    doc::VersionEditor e3(withLight);
    REQUIRE(e3.replaceEntity(renamed));
    c = doc::diff(*withLight, *e3.finish());
    REQUIRE(c.entities.size() == 1);
    CHECK((c.entities[0].kinds & doc::Class) != 0);
    CHECK((c.entities[0].kinds & doc::Light) != 0);                   // it was a light before
}

TEST_CASE("diff: prefab instances and root settings") {
    const auto v = loadDemo();
    core::PrefabInstance p;
    p.id = "i_test";
    p.file = "prefabs/door.map.json";
    doc::VersionEditor add(v);
    REQUIRE(add.addInstance(p));
    const auto with = add.finish();
    doc::ChangeSet c = doc::diff(*v, *with);
    REQUIRE(c.entities.size() == 1);
    CHECK(c.entities[0].instance == "i_test");
    CHECK(c.entities[0].kinds == doc::Prefab);

    core::PrefabInstance moved = with->instances[0]->typed;
    moved.origin = {5, 0, 0};
    doc::VersionEditor mv(with);
    REQUIRE(mv.replaceInstance(moved));
    c = doc::diff(*with, *mv.finish());
    REQUIRE(c.entities.size() == 1);
    CHECK(c.entities[0].kinds == doc::Transform);

    auto root = std::make_shared<doc::DocumentVersion>(*v);
    root->version = doc::nextVersionNumber();
    root->rootRaw["settingsMarker"] = 1;
    CHECK(doc::diff(*v, *root).all);
}

TEST_CASE("version editor: invalid edits are refused and publish nothing") {
    const auto v = loadDemo();
    const size_t w = worldIndex(*v);
    const std::string world = v->entities[w]->typed.id;
    const core::Brush good = v->entities[w]->brushes[0]->brush;

    {   // a brush that is not a closed solid
        core::Brush broken = good;
        broken.faces.pop_back();
        doc::VersionEditor ed(v);
        CHECK_FALSE(ed.replaceBrush(world, broken));
        CHECK_FALSE(ed.ok());
        CHECK(ed.finish() == nullptr);
    }
    {   // unknown ids, duplicate ids
        doc::VersionEditor ed(v);
        CHECK_FALSE(ed.replaceBrush("nope", good));
        CHECK(ed.error().find("no entity") != std::string::npos);
        doc::VersionEditor ed2(v);
        core::Brush other = good;
        other.id = "b_does_not_exist";
        CHECK_FALSE(ed2.replaceBrush(world, other));
        doc::VersionEditor ed3(v);
        CHECK_FALSE(ed3.addBrush(world, good));    // same id again
        core::Entity dup;
        dup.id = world;
        doc::VersionEditor ed4(v);
        CHECK_FALSE(ed4.addEntity(dup));
        doc::VersionEditor ed5(v);
        CHECK_FALSE(ed5.removeBrush(world, "nope"));
        CHECK_FALSE(ed5.removeEntity("nope"));
    }
    {   // a non-convex brush is refused in a worldspawn (the demo brush with one vertex pulled inwards past the plane of a neighbour)
        core::Brush dent = good;
        // collapse every vertex onto the first one: structurally degenerate
        for (auto& p : dent.vertices) p = dent.vertices[0];
        doc::VersionEditor ed(v);
        CHECK_FALSE(ed.replaceBrush(world, dent));
    }
    {   // brush add / remove work and keep the others
        core::Brush copy = good;
        copy.id = "b_copy";
        doc::VersionEditor ed(v);
        REQUIRE(ed.addBrush(world, copy));
        REQUIRE(ed.removeBrush(world, good.id));
        const auto next = ed.finish();
        REQUIRE(next);
        CHECK(next->entities[w]->brushes.size() == v->entities[w]->brushes.size());
        CHECK(next->findBrush(world, "b_copy") >= 0);
        CHECK(next->findBrush(world, good.id) < 0);
        const doc::ChangeSet c = doc::diff(*v, *next);
        CHECK(c.brushes.size() == 2);
        // the node is what its JSON reads back to
        CHECK(core::brushFromJson(next->entities[w]->brushes.back()->raw).id == "b_copy");
    }
}

TEST_CASE("history: undo and redo restore pointer-identical versions") {
    const auto v0 = loadDemo();
    const size_t w = worldIndex(*v0);
    doc::History h(v0);
    CHECK_FALSE(h.canUndo());
    CHECK(h.undoLabel().empty());

    const auto v1 = editBrush(v0, w, 0, [](core::Brush& b) { b.translate({1, 0, 0}); });
    const auto v2 = editBrush(v1, w, 0, [](core::Brush& b) { b.translate({1, 0, 0}); });
    std::vector<doc::ChangeCause> causes;
    std::vector<size_t> brushChanges;
    h.setListener([&](const auto& cur, const doc::ChangeSet& cs, doc::ChangeCause cause) {
        causes.push_back(cause);
        brushChanges.push_back(cs.brushes.size());
        CHECK(cur == h.current());
    });
    REQUIRE(h.commit({"Move", v0, v1}));
    REQUIRE(h.commit({"Nudge", v1, v2}));
    CHECK(h.current() == v2);
    CHECK(h.undoLabel() == "Nudge");
    CHECK(h.undoCount() == 2);

    CHECK(h.undo());
    CHECK(h.current() == v1);
    CHECK(h.redoLabel() == "Nudge");
    CHECK(h.undo());
    CHECK(h.current() == v0);
    CHECK_FALSE(h.undo());
    CHECK(h.redo());
    CHECK(h.current() == v1);
    CHECK(h.redo());
    CHECK(h.current() == v2);
    CHECK_FALSE(h.redo());
    REQUIRE(causes.size() == 6);
    CHECK(causes[0] == doc::ChangeCause::Commit);
    CHECK(causes[2] == doc::ChangeCause::Undo);
    CHECK(causes[5] == doc::ChangeCause::Redo);
    for (size_t n : brushChanges) CHECK(n == 1);                      // undo / redo report the same one-brush change set

    // a new commit after an undo clears redo
    CHECK(h.undo());
    const auto alt = editBrush(v1, w, 0, [](core::Brush& b) { b.translate({0, 5, 0}); });
    REQUIRE(h.commit({"Lift", v1, alt}));
    CHECK_FALSE(h.canRedo());
    // stale or empty commits are refused
    CHECK_FALSE(h.commit({"Stale", v0, v2}));
    CHECK_FALSE(h.commit({"Nothing", alt, alt}));
    CHECK(h.current() == alt);
}

TEST_CASE("history: an interaction merges its commits into one undo step") {
    const auto v0 = loadDemo();
    const size_t w = worldIndex(*v0);
    doc::History h(v0);
    int heard = 0;
    h.setListener([&](const auto&, const doc::ChangeSet&, doc::ChangeCause) { ++heard; });

    h.beginInteraction("Move");
    auto cur = v0;
    for (int i = 0; i < 20; ++i) {
        auto next = editBrush(cur, w, 0, [](core::Brush& b) { b.translate({0.5, 0, 0}); });
        REQUIRE(h.commit({"ignored", cur, next}));
        cur = next;
    }
    CHECK(heard == 20);                                               // every drag step was shown
    CHECK(h.current() == cur);
    CHECK(h.interacting());
    CHECK_FALSE(h.canUndo());                                         // not undoable in the middle of a drag
    CHECK_FALSE(h.undo());
    h.endInteraction();
    CHECK(h.undoCount() == 1);
    CHECK(h.undoLabel() == "Move");
    CHECK(h.undo());
    CHECK(h.current() == v0);                                         // the whole drag in one step
    CHECK(h.redo());
    CHECK(h.current() == cur);

    // an interaction that changed nothing leaves no step
    h.beginInteraction("Idle");
    h.endInteraction();
    CHECK(h.undoCount() == 1);
}

TEST_CASE("history: at most 500 commands, the oldest are dropped; reset clears everything") {
    const auto v0 = loadDemo();
    const size_t w = worldIndex(*v0);
    doc::History h(v0);
    auto cur = v0;
    for (size_t i = 0; i < doc::History::kMaxCommands + 20; ++i) {
        auto next = editBrush(cur, w, 0, [](core::Brush& b) { b.translate({0.01, 0, 0}); });
        REQUIRE(h.commit({"Step " + std::to_string(i), cur, next}));
        cur = next;
    }
    CHECK(h.undoCount() == doc::History::kMaxCommands);
    size_t undone = 0;
    while (h.undo()) ++undone;
    CHECK(undone == doc::History::kMaxCommands);
    CHECK(h.current() != v0);                                         // the first 20 steps can no longer be undone
    bool all = false;
    h.setListener([&](const auto&, const doc::ChangeSet& cs, doc::ChangeCause cause) { all = cs.all && cause == doc::ChangeCause::Reset; });
    h.reset(v0);
    CHECK(all);
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());
    CHECK(h.current() == v0);
}

TEST_CASE("diff on lvl2 with one changed brush costs under a millisecond") {
    if (!fs::exists(kLvl2)) {
        MESSAGE("lvl2 not found, skipped");
        return;
    }
    const doc::LoadResult r = doc::loadDocumentFile(kLvl2, "maps/lvl2.map.json");
    REQUIRE(r.version);
    const auto& v = r.version;
    const size_t w = worldIndex(*v);
    REQUIRE(v->entities[w]->brushes.size() > 50);
    const size_t last = v->entities[w]->brushes.size() / 2;
    const int64_t t0 = base::nowMicros();
    const auto next = editBrush(v, w, last, [](core::Brush& b) { b.translate({0.25, 0, 0}); });
    const double editMs = static_cast<double>(base::nowMicros() - t0) / 1000.0;

    std::vector<double> samples;
    for (int i = 0; i < 101; ++i) {
        const int64_t a = base::nowMicros();
        const doc::ChangeSet c = doc::diff(*v, *next);
        samples.push_back(static_cast<double>(base::nowMicros() - a) / 1000.0);
        REQUIRE(c.brushes.size() == 1);
        REQUIRE(c.entities.empty());
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    MESSAGE("lvl2: " << v->entities[w]->brushes.size() << " brushes in the world, edit " << editMs << " ms, diff median " << median << " ms, max "
                     << samples.back() << " ms");
    CHECK(median < 1.0);
    // identical versions: nothing to look at
    CHECK(doc::diff(*v, *v).empty());
}
