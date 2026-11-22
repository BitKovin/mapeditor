// M11 item 11, U1: the last operation with a number stays adjustable (Adjust dock).
#include <doctest.h>

#include <QDoubleSpinBox>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "ui/adjust_panel.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {4, 4, 4}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

// The thinnest wall of the brushes of the world entity: the smallest extent of any brush along any axis.
double thinnest(const std::shared_ptr<const doc::DocumentVersion>& d) {
    double best = 1e9;
    const auto& e = *d->entities[static_cast<size_t>(d->findEntity("w"))];
    for (const auto& b : e.brushes) {
        core::Vec3 lo, hi;
        if (!b->brush.bounds(lo, hi)) continue;
        best = std::min({best, hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    }
    return best;
}

} // namespace

TEST_CASE("adjust last operation: Make hollow can be redone with another thickness; the history keeps one step; any other edit ends it") {
    ui::MainWindow window(services());
    ui::AdjustPanel* panel = window.adjustPanel();
    REQUIRE(panel);
    window.setDocument(oneBox(), true);
    panel->refreshNow();
    CHECK(window.lastOperation() == nullptr);
    CHECK(panel->title().isEmpty());
    const size_t steps0 = window.history().labels().size();

    window.selection().select(doc::SelRef::brush("w", "a"));
    REQUIRE(window.hollowSelected(0.5));
    REQUIRE(window.lastOperation());
    CHECK(window.lastOperation()->label == "Make hollow");
    CHECK(window.lastOperation()->params.at(0).value == doctest::Approx(0.5));
    CHECK(window.history().labels().size() == steps0 + 1);
    CHECK(thinnest(window.document()) == doctest::Approx(0.5));
    panel->refreshNow();
    CHECK(panel->title() == QStringLiteral("Make hollow"));
    QDoubleSpinBox* thickness = panel->field(QStringLiteral("thickness"));
    REQUIRE(thickness);
    CHECK(thickness->value() == doctest::Approx(0.5));

    // the field changes it: the same number of steps, the walls are thinner
    thickness->setValue(0.25);
    REQUIRE(panel->applyNow());
    CHECK(window.history().labels().size() == steps0 + 1);
    CHECK(thinnest(window.document()) == doctest::Approx(0.25));
    REQUIRE(window.lastOperation());
    CHECK(window.lastOperation()->params.at(0).value == doctest::Approx(0.25));
    // it can be done again from the result
    CHECK(window.adjustLastOperation({1.0}));
    CHECK(thinnest(window.document()) == doctest::Approx(1.0));
    CHECK(window.history().labels().size() == steps0 + 1);
    // wrong number of values: nothing happens
    CHECK_FALSE(window.adjustLastOperation({}));
    CHECK(thinnest(window.document()) == doctest::Approx(1.0));
    // too thick for the box: refused, the last good result stays and can still be adjusted
    CHECK_FALSE(window.adjustLastOperation({50.0}));
    CHECK(thinnest(window.document()) == doctest::Approx(1.0));
    REQUIRE(window.lastOperation());

    // undo ends it, redo gives it back
    window.undo();
    CHECK(window.lastOperation() == nullptr);
    window.redo();
    CHECK(window.lastOperation() != nullptr);
    // another edit ends it
    window.selectAllItems();
    REQUIRE(window.flipSelected(0));
    CHECK(window.lastOperation() == nullptr);
    window.setDocument(oneBox(), true);
    CHECK(window.lastOperation() == nullptr);
    panel->refreshNow();
    CHECK(panel->title().isEmpty());
}

TEST_CASE("adjust last operation: the dock has a View menu toggle") {
    ui::MainWindow window(services());
    CHECK(window.registry().find("view.adjust.toggle") != nullptr);
    CHECK(window.adjustDock() != nullptr);
}
