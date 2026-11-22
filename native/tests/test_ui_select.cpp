// M11 item 10: Select all / Invert / Select none and the arrow nudge of the object tool (tools/select_ops, ui/select_actions): the pure helpers, the actions and the keys
// through the router. Offscreen platform, the window is never shown.
#include <doctest.h>

#include <QApplication>
#include <QStatusBar>

#include <algorithm>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/select_ops.h"
#include "tools/visibility.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;

namespace {

std::shared_ptr<const doc::DocumentVersion> mapDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {4, 0, 0}, {6, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity g;
    g.id = "g";
    g.classname = "func_group";
    g.brushes.push_back(core::Brush::makeBox("gb", {8, 0, 0}, {10, 2, 2}, "dev/grey"));
    g.brushes.push_back(core::Brush::makeBox("gc", {12, 0, 0}, {14, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(g));
    core::Entity p;
    p.id = "p";
    p.classname = "info_player_start";
    REQUIRE(ed.addEntity(p));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

struct Fixture {
    ui::MainWindow window;
    Fixture() : window(services()) { window.setDocument(mapDoc(), true); }
    bool key(const std::string& name, uint8_t mods = 0) {
        window.router().contexts().set({"window:main", "area:viewport", "view:perspective", "view:fly", "tool:select"});
        ui::UiEnv env{&window, nullptr};
        actions::ActionEnv ae{&env};
        actions::InputEvent ev;
        ev.type = actions::InputEvent::Type::KeyPress;
        ev.key = name;
        ev.scan = name;
        ev.mods = mods;
        const bool handled = window.router().handle(ev, ae).handled;
        ev.type = actions::InputEvent::Type::KeyRelease;
        window.router().handle(ev, ae);
        return handled;
    }
    const core::Brush* brush(const std::string& id) const {
        for (const auto& e : window.document()->entities)
            for (const auto& b : e->brushes)
                if (b->brush.id == id) return &b->brush;
        return nullptr;
    }
    double minOf(const std::string& id, int axis) const {
        const core::Brush* b = brush(id);
        double m = 1e9;
        if (b)
            for (const auto& v : b->vertices) m = std::min(m, axis == 0 ? v.x : axis == 1 ? v.y : v.z);
        return m;
    }
};

} // namespace

TEST_CASE("select ops: everything selectable, and the inverse of a selection") {
    const auto d = mapDoc();
    const std::vector<SelRef> all = tools::selectableItems(*d);
    REQUIRE(all.size() == 5);   // a, b, gb, gc (brushes) and the point entity p
    CHECK(all[0] == SelRef::brush("w", "a"));
    CHECK(all[4] == SelRef::entity("p"));
    // the whole group counts as its brushes; a face counts as its brush
    const std::vector<SelRef> rest = tools::invertedItems(*d, {SelRef::entity("g"), SelRef::face("w", "a", 0)});
    REQUIRE(rest.size() == 2);
    CHECK(rest[0] == SelRef::brush("w", "b"));
    CHECK(rest[1] == SelRef::entity("p"));
    CHECK(tools::invertedItems(*d, all).empty());
    CHECK(tools::invertedItems(*d, {}).size() == 5);
}

TEST_CASE("select ops: the nudge follows the dominant ground axis of the camera") {
    const tools::V3 right{0.1, 0, 1}, forward{1, 0, -0.1};   // looking along +X, screen right = +Z
    CHECK(tools::nudgeDirection("right", right, forward) == tools::V3{0, 0, 1});
    CHECK(tools::nudgeDirection("left", right, forward) == tools::V3{0, 0, -1});
    CHECK(tools::nudgeDirection("up", right, forward) == tools::V3{1, 0, 0});
    CHECK(tools::nudgeDirection("down", right, forward) == tools::V3{-1, 0, 0});
    CHECK(tools::nudgeDirection("pageup", right, forward) == tools::V3{0, 1, 0});
    CHECK(tools::nudgeDirection("pagedown", right, forward) == tools::V3{0, -1, 0});
    CHECK(tools::nudgeDirection("home", right, forward) == tools::V3{0, 0, 0});
}

TEST_CASE("select all / invert / none: the actions and their keys") {
    Fixture f;
    CHECK_FALSE(f.key("Escape"));   // nothing selected: Esc is free for other uses
    CHECK(f.key("A", actions::kCtrl));
    CHECK(f.window.selection().items().size() >= 3);   // the group of two brushes may be one entity entry
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Selected")));
    CHECK(f.key("Escape"));
    CHECK(f.window.selection().empty());

    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(f.key("I", actions::kCtrl));
    std::vector<std::string> ids;
    for (const SelRef& s : f.window.selection().items()) ids.push_back(s.brushId.empty() ? s.entityId : s.brushId);
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<std::string>{"b", "g", "p"});   // the two brushes of the group are the whole group
    CHECK(f.key("A", actions::kAlt));
    CHECK(f.window.selection().empty());
}

TEST_CASE("select all leaves hidden things out") {
    Fixture f;
    f.window.setHideRules({{"func_group", true, true}});
    f.window.runAction("edit.select_all");
    CHECK_FALSE(f.window.selection().empty());
    for (const SelRef& s : f.window.selection().items()) CHECK(s.entityId != "g");
}

TEST_CASE("arrows nudge the selection by a grid step, PageUp / PageDown move it up / down, each one undo step") {
    Fixture f;
    CHECK_FALSE(f.key("Right"));   // nothing selected
    f.window.selection().select(SelRef::brush("w", "a"));
    const double x0 = f.minOf("a", 0), y0 = f.minOf("a", 1);
    CHECK(f.key("Right"));
    CHECK(f.minOf("a", 0) == doctest::Approx(x0 + 0.25));   // no viewport: looking along -Z, screen right = +X, a grid step of 0.25
    CHECK(f.key("Left"));
    CHECK(f.minOf("a", 0) == doctest::Approx(x0));
    CHECK(f.key("PageUp"));
    CHECK(f.minOf("a", 1) == doctest::Approx(y0 + 0.25));
    CHECK(f.key("PageDown"));
    CHECK(f.minOf("a", 1) == doctest::Approx(y0));
    CHECK(f.minOf("b", 0) == doctest::Approx(4));   // the rest stays
    // fine = an eighth of the step, through the action's parameter
    CHECK(f.window.runAction("edit.nudge", {{"key", "right"}, {"fine", true}}));
    CHECK(f.minOf("a", 0) == doctest::Approx(x0 + 0.25 / 8));
    f.window.undo();
    CHECK(f.minOf("a", 0) == doctest::Approx(x0));
    // a point entity moves its origin (forward = -Z)
    f.window.selection().select(SelRef::entity("p"));
    CHECK(f.key("Up"));
    CHECK(f.window.document()->entities.back()->typed.origin.z == doctest::Approx(-0.25));
}

TEST_CASE("Ctrl+H and Ctrl+M of the object tool reach the brush operations") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "a"));
    CHECK(f.key("H", actions::kCtrl));   // hollow: one box becomes walls
    CHECK(f.window.document()->entities[0]->brushes.size() > 2);
    f.window.undo();
    f.window.selection().selectMany(*f.window.document(), {SelRef::brush("w", "a"), SelRef::brush("w", "b")});
    CHECK(f.key("M", actions::kCtrl));   // merge: refused (the two boxes do not make a convex shape), but the key was the merge's
    CHECK(f.window.statusBar()->currentMessage().size() > 0);
}

TEST_CASE("flip and rotate 90 degrees: about the centre of the selection, one undo step, flip needs a brush") {
    Fixture f;
    f.window.selection().selectMany(*f.window.document(), {SelRef::brush("w", "a"), SelRef::brush("w", "b")});   // x 0..2 and 4..6: the centre is x = 3
    CHECK(f.window.runAction("brush.flip", {{"axis", "x"}}));
    CHECK(f.minOf("a", 0) == doctest::Approx(4));
    CHECK(f.minOf("b", 0) == doctest::Approx(0));
    CHECK(f.brush("a")->signedVolume() > 0);   // the faces were turned with the mirror: still a valid solid
    f.window.undo();
    CHECK(f.minOf("a", 0) == doctest::Approx(0));
    CHECK(f.window.runAction("brush.rotate90", {{"axis", "y"}, {"dir", 1}}));   // a long wall along X becomes one along Z
    CHECK(f.minOf("a", 0) == doctest::Approx(2));
    CHECK(f.minOf("a", 2) != doctest::Approx(0));
    f.window.undo();
    CHECK(f.window.runAction("brush.rotate90", {{"axis", "y"}, {"dir", -1}}));
    CHECK(f.minOf("a", 0) == doctest::Approx(2));
    f.window.undo();
    // a point entity alone: nothing to flip
    f.window.selection().select(SelRef::entity("p"));
    f.window.runAction("brush.flip", {{"axis", "x"}});
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("select a brush first")));
}

namespace {

std::shared_ptr<const doc::DocumentVersion> similarDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {4, 0, 0}, {6, 2, 2}, "dev/wall"));
    REQUIRE(ed.addEntity(w));
    core::Entity g;
    g.id = "g";
    g.classname = "func_group";
    g.brushes.push_back(core::Brush::makeBox("gb", {8, 0, 0}, {10, 2, 2}, "dev/wall"));
    REQUIRE(ed.addEntity(g));
    core::Entity l1, l2;
    l1.id = "l1";
    l1.classname = "light_point";
    l2.id = "l2";
    l2.classname = "light_point";
    REQUIRE(ed.addEntity(l1));
    REQUIRE(ed.addEntity(l2));
    for (const auto& [id, file] : {std::pair<const char*, const char*>{"i1", "prefabs/door.map.json"}, {"i2", "prefabs/door.map.json"}, {"i3", "prefabs/crate.map.json"}}) {
        core::PrefabInstance p;
        p.id = id;
        p.file = file;
        REQUIRE(ed.addInstance(p));
    }
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::vector<std::string> idsOf(const std::vector<SelRef>& list) {
    std::vector<std::string> out;
    for (const SelRef& s : list) out.push_back(s.isInstance() ? s.instancePath[0] : s.brushId.empty() ? s.entityId : s.brushId);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("select similar: same texture, same class, same prefab (pure)") {
    const auto d = similarDoc();
    using tools::SimilarKind;
    // texture: the face of the brush "a" says dev/grey: only that brush; "b" says dev/wall: it and the brush of the group
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::brush("w", "a")}, SimilarKind::Texture).found) == std::vector<std::string>{"a"});
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::brush("w", "b")}, SimilarKind::Texture).found) == std::vector<std::string>{"b", "gb"});
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::face("w", "b", 0)}, SimilarKind::Texture).found) == std::vector<std::string>{"b", "gb"});
    CHECK(tools::selectSimilar(*d, {SelRef::entity("l1")}, SimilarKind::Texture).found.empty());
    // class: the worldspawn brushes; the lights; the group's brush alone
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::brush("w", "a")}, SimilarKind::Class).found) == std::vector<std::string>{"a", "b"});
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::entity("l1")}, SimilarKind::Class).found) == std::vector<std::string>{"l1", "l2"});
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::entity("g")}, SimilarKind::Class).found) == std::vector<std::string>{"gb"});
    // prefab: the two doors, not the crate
    CHECK(idsOf(tools::selectSimilar(*d, {SelRef::instance("i1")}, SimilarKind::Prefab).found) == std::vector<std::string>{"i1", "i2"});
    const tools::SimilarSelect none = tools::selectSimilar(*d, {SelRef::brush("w", "a")}, SimilarKind::Prefab);
    CHECK(none.found.empty());
    CHECK(none.message.find("select a prefab instance first") != std::string::npos);
}

TEST_CASE("select similar: the action in the window, hidden things stay out") {
    ui::MainWindow window(services());
    window.setDocument(similarDoc(), true);
    window.selection().select(SelRef::brush("w", "b"));
    CHECK(window.runAction("edit.select_similar", {{"kind", "texture"}}));
    {
        const std::vector<std::string> got = idsOf(window.selection().items());
        INFO("selected: " << (got.empty() ? std::string("-") : got[0]) << " " << got.size());
        CHECK(got == std::vector<std::string>{"b", "gb"});
    }   
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("same texture")));
    window.selection().select(SelRef::entity("l1"));
    window.setHideRules({{"light_point", true, true}});
    CHECK_FALSE(window.selectSimilar("class"));   // both lights are hidden: nothing is selected
    window.setHideRules({});
    window.selection().select(SelRef::entity("l1"));
    CHECK(window.selectSimilar("class"));
    CHECK(idsOf(window.selection().items()) == std::vector<std::string>{"l1", "l2"});
    window.selection().clear();
    window.runAction("edit.select_similar", {{"kind", "class"}});   // nothing selected: nothing to be similar to
    CHECK(window.selection().empty());
}
