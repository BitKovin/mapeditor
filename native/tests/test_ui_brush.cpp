// T11.2b: the Radiant brush operations on the document and in the main window: clipper, hollow, merge, snap, select inside / touching (ports of the actions.ts parts and
// their tests). Offscreen platform, the window is never shown, no GPU (the clicks of the clipper in a viewport need one; the points are placed through the API).
#include <doctest.h>

#include <QApplication>
#include <QStatusBar>

#include <cmath>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "tools/brush_edit.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;

namespace {

std::shared_ptr<const doc::DocumentVersion> roomDoc() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("room", {0, 0, 0}, {10, 10, 10}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("in1", {1, 1, 1}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("in2", {3, 3, 3}, {4, 4, 4}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("out", {20, 0, 0}, {21, 1, 1}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("edge", {9, 9, 9}, {11, 11, 11}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity f;
    f.id = "f";
    f.classname = "func_group";
    f.brushes.push_back(core::Brush::makeBox("grp", {5, 5, 5}, {6, 6, 6}, "dev/grey"));
    REQUIRE(ed.addEntity(f));
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
    Fixture() : window(services()) { window.setDocument(roomDoc(), true); }
    size_t brushCount() const {
        size_t n = 0;
        for (const auto& e : window.document()->entities) n += e->brushes.size();
        return n;
    }
    const core::Brush* brush(const std::string& id) const {
        for (const auto& e : window.document()->entities)
            for (const auto& b : e->brushes)
                if (b->brush.id == id) return &b->brush;
        return nullptr;
    }
    std::vector<std::string> selectedIds() {
        std::vector<std::string> out;
        for (const SelRef& s : window.selection().items()) out.push_back(s.brushId);
        std::sort(out.begin(), out.end());
        return out;
    }
    bool key(const std::string& name, uint8_t mods = 0) {
        std::vector<std::string> contexts = {"window:main", "area:viewport", "view:perspective", "view:fly", "tool:select"};
        if (window.clipperActive()) contexts.push_back("brush:clipper");
        window.router().contexts().set(contexts);
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
    void place(const core::Vec3& a, const core::Vec3& b, const core::Vec3& c) {
        window.addClipperPoint(a, {0, 0, -1});
        window.addClipperPoint(b, {0, 0, -1});
        window.addClipperPoint(c, {0, 0, -1});
    }
};

double volume(const core::Brush* b) { return b ? b->signedVolume() : -1; }

} // namespace

TEST_CASE("select inside / touching: the other brushes in the volume of the selection, across entities") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "room"));
    CHECK(f.window.runAction("brush.select_inside"));
    CHECK(f.selectedIds() == std::vector<std::string>{"grp", "in1", "in2"});
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("3 brushes inside")));
    f.window.selection().select(SelRef::brush("w", "room"));
    CHECK(f.window.runAction("brush.select_touching"));
    CHECK(f.selectedIds() == std::vector<std::string>{"edge", "grp", "in1", "in2"});
    // nothing around the selected brush: the selection stays
    f.window.selection().select(SelRef::brush("w", "out"));
    CHECK_FALSE(f.window.selectByVolume(tools::VolumeMode::Touching));
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("no other brush")));
    CHECK(f.selectedIds() == std::vector<std::string>{"out"});
}

TEST_CASE("clipper: three points make the plane, Enter keeps the back side as one undo step, Esc and an empty selection leave the clipper") {
    Fixture f;
    CHECK_FALSE(f.window.startClipper());   // nothing selected
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("select a brush first")));
    f.window.selection().select(SelRef::brush("w", "room"));
    CHECK(f.key("X"));
    REQUIRE(f.window.clipperActive());
    const auto before = f.window.document();
    CHECK(f.key("Enter"));   // no plane yet: the key is the clipper's, nothing happens
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("at least two points")));
    CHECK(f.window.document() == before);
    f.place({5, 0, 0}, {5, 1, 0}, {5, 0, 1});   // the normal points to +X: the back side is x <= 5
    CHECK(f.window.clipperPoints().size() == 3);
    CHECK(f.key("Enter"));
    CHECK_FALSE(f.window.clipperActive());
    CHECK(volume(f.brush("room")) == doctest::Approx(500));
    f.window.history().undo();
    CHECK(f.window.document() == before);
    CHECK(f.key("X"));
    CHECK(f.key("Escape"));
    CHECK_FALSE(f.window.clipperActive());
    CHECK(f.window.document() == before);
}

TEST_CASE("clipper: Shift+Enter splits into two brushes, Ctrl+Enter keeps the front; two points use the view direction") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "room"));
    REQUIRE(f.window.startClipper());
    f.place({5, 0, 0}, {5, 1, 0}, {5, 0, 1});
    const size_t n = f.brushCount();
    CHECK(f.key("Enter", actions::kShift));
    CHECK(f.brushCount() == n + 1);
    CHECK(volume(f.brush("room")) == doctest::Approx(500));
    f.window.history().undo();
    f.window.selection().select(SelRef::brush("w", "room"));
    REQUIRE(f.window.startClipper());
    f.place({5, 0, 0}, {5, 1, 0}, {5, 0, 1});
    CHECK(f.key("Enter", actions::kCtrl));
    CHECK(volume(f.brush("room")) == doctest::Approx(500));
    const core::Brush* room = f.brush("room");
    REQUIRE(room);
    double minX = 1e300;
    for (const core::Vec3& v : room->vertices) minX = std::min(minX, v.x);
    CHECK(minX == doctest::Approx(5));
    // two points: the plane contains the view direction (0, 0, -1): here the points (2,0,0) and (2,1,0) make the plane x = 2
    f.window.selection().select(SelRef::brush("w", "room"));
    REQUIRE(f.window.startClipper());
    f.window.addClipperPoint({7, 0, 0}, {0, 0, -1});
    f.window.addClipperPoint({7, 1, 0}, {0, 0, -1});
    const auto pl = f.window.clipperPoints();
    CHECK(pl.size() == 2);
    CHECK(f.window.clipperApply(tools::Keep::Back));
    CHECK(volume(f.brush("room")) == doctest::Approx(300));   // the normal is -X here: the back side is x >= 7
}

TEST_CASE("clipper: a plane that misses the brush, or a selection of concave brushes, says why") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "room"));
    REQUIRE(f.window.startClipper());
    f.place({50, 0, 0}, {50, 1, 0}, {50, 0, 1});   // far behind the brush: everything is on the back side
    const auto before = f.window.document();
    CHECK_FALSE(f.window.clipperApply(tools::Keep::Back));
    CHECK(f.window.document() == before);
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("does not cut")));
    CHECK(f.window.clipperActive());   // stays armed
}

TEST_CASE("hollow: a box becomes six walls as one undo step; a wall as thick as the brush is refused") {
    Fixture f;
    f.window.selection().select(SelRef::brush("w", "room"));
    const auto before = f.window.document();
    const size_t n = f.brushCount();
    CHECK(f.window.runAction("brush.hollow", {{"thickness", 1.0}}));
    CHECK(f.brushCount() == n + 5);
    double total = 0;
    for (const char* id : {"room"}) total += volume(f.brush(id));
    CHECK(total > 0);
    f.window.history().undo();
    CHECK(f.window.document() == before);
    f.window.selection().select(SelRef::brush("w", "in1"));
    CHECK_FALSE(f.window.hollowSelected(5));
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("no brush can take a wall")));
    CHECK(f.window.document() == before);
}

TEST_CASE("merge: touching boxes become one brush that keeps the first id; brushes that do not form a convex shape are refused") {
    Fixture f;
    doc::VersionEditor ed(f.window.document());
    REQUIRE(ed.addBrush("w", core::Brush::makeBox("a", {30, 0, 0}, {32, 2, 2}, "dev/grey")));
    REQUIRE(ed.addBrush("w", core::Brush::makeBox("c", {32, 0, 0}, {34, 2, 2}, "dev/grey")));
    f.window.setDocument(ed.finish(), false);
    const size_t n = f.brushCount();
    f.window.selection().selectMany(*f.window.document(), {SelRef::brush("w", "a"), SelRef::brush("w", "c")});
    CHECK(f.window.runAction("brush.merge"));
    CHECK(f.brushCount() == n - 1);
    CHECK(volume(f.brush("a")) == doctest::Approx(16));
    CHECK(f.brush("c") == nullptr);
    CHECK(f.selectedIds() == std::vector<std::string>{"a"});
    // an L shape (two boxes stacked offset) is not convex
    f.window.selection().selectMany(*f.window.document(), {SelRef::brush("w", "in1"), SelRef::brush("w", "out")});
    const auto before = f.window.document();
    CHECK_FALSE(f.window.mergeSelected());
    CHECK(f.window.document() == before);
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("do not form a convex shape")));
}

TEST_CASE("snap: the vertices of the selected brushes land on the grid; already snapped brushes say so") {
    Fixture f;
    doc::VersionEditor ed(f.window.document());
    REQUIRE(ed.addBrush("w", core::Brush::makeBox("odd", {40.13, 0.02, -0.1}, {43.9, 4.05, 4.2}, "dev/grey")));
    f.window.setDocument(ed.finish(), false);
    f.window.selection().select(SelRef::brush("w", "odd"));
    CHECK(f.window.runAction("brush.snap"));
    const core::Brush* b = f.brush("odd");
    REQUIRE(b);
    for (const core::Vec3& v : b->vertices)
        for (double x : {v.x, v.y, v.z}) CHECK(std::abs(x / 0.25 - std::round(x / 0.25)) < 1e-9);
    CHECK_FALSE(f.window.snapSelected());
    CHECK(f.window.statusBar()->currentMessage().contains(QStringLiteral("Already on the grid")));
}

TEST_CASE("brush operations: keys and menu") {
    Fixture f;
    CHECK(f.window.keymap().displayText("brush.clipper") == "X");
    CHECK(f.window.keymap().displayText("brush.clip_back") == "Enter");
    f.window.selection().select(SelRef::brush("w", "room"));
    CHECK(f.window.runAction("brush.clipper"));
    CHECK(f.window.clipperActive());
    CHECK(f.window.runAction("brush.clipper"));
    CHECK_FALSE(f.window.clipperActive());
    // the clipper is off while mesh mode owns the brush
    CHECK(f.window.enterMeshMode());
    CHECK_FALSE(f.window.startClipper());
}
