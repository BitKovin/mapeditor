// M11 item 11, U2: the Tool dock shows the settings of the active tool (snapping, gizmo, shape) with a real window and viewports (native_gui_tests).
#include <doctest.h>

#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QToolButton>

#include "base/jobs.h"
#include "doc/io.h"
#include "gui_support.h"
#include "ui/tool_options_panel.h"

using namespace native;
using namespace guitest;

namespace {

template <class T>
T* part(ui::ToolOptionsPanel* p, const char* name) {
    return qobject_cast<T*>(p->part(QString::fromLatin1(name)));
}

void choose(QComboBox* c, const QString& text) {
    const int i = c->findText(text);
    REQUIRE(i >= 0);
    c->setCurrentIndex(i);
    emit c->activated(i);
}

} // namespace

TEST_CASE("tool options: snapping and gizmo follow the active viewport, the shape and its numbers follow the box tool" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    ui::MainWindow window(sv);
    window.resize(900, 650);
    window.show();
    REQUIRE(waitUntil([&] { return allReady(window); }));
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    window.setDocument(r.version, true);
    render::ViewportWindow* vp = window.activeViewport();
    REQUIRE(vp);
    ui::ToolOptionsPanel* panel = window.toolOptionsPanel();
    REQUIRE(panel);
    panel->refreshNow();

    CHECK(panel->title() == QStringLiteral("Tool: Select"));
    CHECK_FALSE(panel->part(QStringLiteral("group:select"))->isHidden());
    CHECK(panel->part(QStringLiteral("group:box"))->isHidden());
    auto* snap = part<QCheckBox>(panel, "snap");
    REQUIRE(snap);
    CHECK(snap->isChecked());

    // the fields set the viewport's settings; the settings show in the fields
    snap->click();
    CHECK_FALSE(vp->rig().settings().snapEnabled);
    choose(part<QComboBox>(panel, "grid"), QStringLiteral("0.25 m"));
    CHECK(vp->rig().settings().gridSize == doctest::Approx(0.25));
    choose(part<QComboBox>(panel, "rotation"), QString::fromUtf8("45\xC2\xB0"));
    CHECK(vp->rig().settings().rotSnap == doctest::Approx(45));
    window.runAction("snap.grid_size", nlohmann::json{{"value", 2.0}});
    pumpFor(50);
    CHECK(part<QComboBox>(panel, "grid")->currentText() == QStringLiteral("2 m"));
    window.runAction("snap.toggle");
    pumpFor(50);
    CHECK(snap->isChecked());

    auto* rotate = part<QToolButton>(panel, "gizmo:rotate");
    REQUIRE(rotate);
    rotate->click();
    CHECK(vp->rig().settings().gizmo == render::GizmoMode::Rotate);
    window.runAction("gizmo.mode", nlohmann::json{{"mode", "scale"}});
    pumpFor(50);
    CHECK(part<QToolButton>(panel, "gizmo:scale")->isChecked());
    CHECK_FALSE(rotate->isChecked());

    // the box tool: the shape, and only the numbers of that shape
    window.setShape(tools::ShapeId::Cylinder);
    panel->refreshNow();
    CHECK(panel->title() == QStringLiteral("Tool: Box (cylinder)"));
    CHECK(panel->part(QStringLiteral("group:select"))->isHidden());
    CHECK_FALSE(panel->part(QStringLiteral("group:box"))->isHidden());
    CHECK_FALSE(panel->part(QStringLiteral("param:sides"))->parentWidget()->isHidden());
    CHECK(panel->part(QStringLiteral("param:steps"))->parentWidget()->isHidden());
    auto* sides = part<QSpinBox>(panel, "param:sides");
    REQUIRE(sides);
    CHECK(sides->value() == window.shapeParams().sides);
    sides->setValue(20);
    CHECK(window.shapeParams().sides == 20);
    choose(part<QComboBox>(panel, "shape"), QStringLiteral("Stairs"));
    CHECK(window.shape() == tools::ShapeId::Stairs);
    panel->refreshNow();
    CHECK_FALSE(panel->part(QStringLiteral("param:steps"))->parentWidget()->isHidden());
    CHECK(panel->part(QStringLiteral("param:sides"))->parentWidget()->isHidden());
    // the Place panel's numbers are the same ones
    window.setShapeParams([&] {
        tools::ShapeParams p = window.shapeParams();
        p.steps = 5;
        return p;
    }());
    panel->refreshNow();
    CHECK(part<QSpinBox>(panel, "param:steps")->value() == 5);

    window.setTool(ui::MainWindow::Tool::Select);
    panel->refreshNow();
    CHECK(panel->title() == QStringLiteral("Tool: Select"));
    CHECK(window.registry().find("view.tool_options.toggle") != nullptr);
    CHECK(window.toolOptionsDock() != nullptr);
}
