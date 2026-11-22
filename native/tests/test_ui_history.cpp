// M11 item 11, U3: the History dock and doc::History::goTo. Offscreen platform, the window is never shown.
#include <doctest.h>

#include <QApplication>
#include <QListWidget>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "ui/app_actions.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"

using namespace native;
using doc::SelRef;

namespace {

std::shared_ptr<const doc::DocumentVersion> twoBoxes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    w.brushes.push_back(core::Brush::makeBox("b", {4, 0, 0}, {6, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

double minX(const ui::MainWindow& w, const std::string& id) {
    double m = 1e9;
    for (const auto& e : w.document()->entities)
        for (const auto& b : e->brushes)
            if (b->brush.id == id)
                for (const auto& v : b->brush.vertices) m = std::min(m, v.x);
    return m;
}

} // namespace

TEST_CASE("history: goTo moves to any state in one step, labels list the steps, a new edit drops the redo steps") {
    doc::History h(twoBoxes());
    int heard = 0;
    h.setListener([&](const std::shared_ptr<const doc::DocumentVersion>&, const doc::ChangeSet&, doc::ChangeCause) { ++heard; });
    const auto v0 = h.current();
    std::vector<std::shared_ptr<const doc::DocumentVersion>> states{v0};
    for (const char* label : {"One", "Two", "Three"}) {
        doc::VersionEditor ed(h.current());
        core::Entity e;
        e.id = std::string("e") + label;
        e.classname = "info_player_start";
        REQUIRE(ed.addEntity(e));
        const auto next = ed.finish();
        REQUIRE(h.commit({label, h.current(), next}));
        states.push_back(h.current());
    }
    CHECK(h.position() == 3);
    CHECK(h.labels() == std::vector<std::string>{"One", "Two", "Three"});
    heard = 0;
    CHECK(h.goTo(0));
    CHECK(heard == 1);   // one announcement, not one per step
    CHECK(h.current() == states[0]);
    CHECK(h.position() == 0);
    CHECK(h.canRedo());
    CHECK(h.labels() == std::vector<std::string>{"One", "Two", "Three"});   // the redo steps stay in the list
    CHECK(h.goTo(2));
    CHECK(h.current() == states[2]);
    CHECK(h.undoLabel() == "Two");
    CHECK(h.redoLabel() == "Three");
    CHECK_FALSE(h.goTo(2));   // already there
    CHECK_FALSE(h.goTo(4));   // out of range
    CHECK(h.goTo(3));
    CHECK(h.current() == states[3]);
    // an edit after going back drops what was ahead
    CHECK(h.goTo(1));
    doc::VersionEditor ed(h.current());
    core::Entity e;
    e.id = "eNew";
    e.classname = "info_player_start";
    REQUIRE(ed.addEntity(e));
    REQUIRE(h.commit({"New", h.current(), ed.finish()}));
    CHECK(h.labels() == std::vector<std::string>{"One", "New"});
    // not during a drag
    h.beginInteraction("Drag");
    CHECK_FALSE(h.goTo(0));
    h.endInteraction();
}

TEST_CASE("history panel: lists the steps, marks the current one, a click jumps there") {
    ui::MainWindow window(services());
    window.setDocument(twoBoxes(), true);
    ui::HistoryPanel* p = window.historyPanel();
    REQUIRE(p);
    CHECK(p->rows() == QStringList{"Start"});
    window.selection().select(SelRef::brush("w", "a"));
    REQUIRE(window.runAction("edit.nudge", {{"key", "right"}}));
    window.selection().selectMany(*window.document(), {SelRef::brush("w", "a"), SelRef::brush("w", "b")});
    REQUIRE(window.runAction("brush.flip", {{"axis", "x"}}));
    p->refreshNow();
    CHECK(p->rows() == QStringList{"Start", "Nudge", "Flip X"});
    CHECK(p->currentRow() == 2);
    const double flipped = minX(window, "a");
    CHECK(p->jumpTo(1));
    CHECK(minX(window, "a") == doctest::Approx(0.25));   // the nudge, not the flip
    p->refreshNow();
    CHECK(p->currentRow() == 1);
    CHECK(p->rows().size() == 3);   // Flip X stays as a redo step
    CHECK(p->jumpTo(0));
    CHECK(minX(window, "a") == doctest::Approx(0));
    CHECK(p->jumpTo(2));
    CHECK(minX(window, "a") == doctest::Approx(flipped));
    CHECK_FALSE(p->jumpTo(2));
    CHECK_FALSE(p->jumpTo(9));
    // the panel follows a new document
    window.setDocument(twoBoxes(), true);
    p->refreshNow();
    CHECK(p->rows() == QStringList{"Start"});
}

TEST_CASE("history panel: the dock has a View menu toggle") {
    ui::MainWindow window(services());
    CHECK(window.registry().find("view.history.toggle") != nullptr);
    CHECK(window.historyDock() != nullptr);
}
