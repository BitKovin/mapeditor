// T4.4 (Qt side): the Outliner, Details and Place docks in a real window (native_gui_tests).
#include <doctest.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include "base/jobs.h"
#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "ui/details_panel.h"
#include "ui/outliner_panel.h"
#include "ui/place_panel.h"

using namespace native;
using namespace guitest;

namespace {

const char* kEnt = R"(
<point name="nav_point" color="0 0.5 0" box="-8 -8 0 8 8 32">
Nav point.
<real key="waitTime" name="Wait Time" default="0">w</real>
<target key="target" name="Next nav point">t</target>
<boolean key="loop" name="Loop">l</boolean>
</point>
<point name="info_player_start" color="0.2 0.8 0.2" box="-16 -16 -24 16 16 32">Spawn.</point>
<point name="light_spot" color="1 1 0" box="-8 -8 -8 8 8 8">A spot.</point>
<point name="light_q3" color="1 1 0" box="-8 -8 -8 8 8 8">A classic light.</point>
<group name="func_door" color="0 0.5 0.8">A door.</group>
)";

void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

std::shared_ptr<const doc::DocumentVersion> testMap() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity t;
    t.id = "t";
    t.classname = "trigger_hurt";
    t.brushes.push_back(core::Brush::makeBox("tb", {3, -1, -1}, {5, 1, 1}, "dev/grey"));
    t.brushes.push_back(core::Brush::makeBox("tc", {6, -1, -1}, {7, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(t));
    core::Entity n;
    n.id = "n";
    n.classname = "nav_point";
    n.keyvalues["targetname"] = "nav1";
    REQUIRE(ed.addEntity(n));
    core::Entity n2;
    n2.id = "n2";
    n2.classname = "nav_point";
    n2.keyvalues["targetname"] = "nav2";
    REQUIRE(ed.addEntity(n2));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

} // namespace

TEST_CASE("Outliner, Details and Place panels edit the document through the main window" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir dir;
    write(dir.path() + "/entities.ent", kEnt);
    write(dir.path() + "/prefabs/door.map.json", R"({"schemaVersion": "0.2", "entities": []})");
    write(dir.path() + "/prefabs/sub/window.map.json", R"({"schemaVersion": "0.2", "entities": []})");
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
    {
        ui::MainWindow window(sv);
        window.resize(1100, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(testMap(), true);
        REQUIRE(window.outliner());
        REQUIRE(window.details());
        REQUIRE(window.place());
        REQUIRE(window.entityDefinitions());
        REQUIRE(window.entityDefinitions()->find("nav_point"));
        ui::OutlinerPanel& outliner = *window.outliner();
        ui::DetailsPanel& details = *window.details();
        ui::PlacePanel& place = *window.place();
        pumpFor(100);
        outliner.refreshNow();

        const auto keys = [&] {
            std::vector<std::string> out;
            for (const auto& r : outliner.model().rows) out.push_back(r.key);
            return out;
        };

        SUBCASE("outliner: rows, fold, search, selection and the eye") {
            CHECK(keys() == std::vector<std::string>{"e:w", "e:t", "e:n", "e:n2"});
            outliner.clickFold("e:t");
            CHECK(keys() == std::vector<std::string>{"e:w", "e:t", "b:t|tb", "b:t|tc", "e:n", "e:n2"});
            CHECK(window.selection().empty());   // the fold arrow does not select
            outliner.clickFold("e:t");

            outliner.clickRow("e:n");
            REQUIRE(window.selection().primary());
            CHECK(window.selection().primary()->entityId == "n");
            outliner.refreshNow();
            CHECK(outliner.model().rows[static_cast<size_t>(outliner.rowOfKey("e:n"))].selected);
            // a brush that is the only one of its entity is the entity
            outliner.clickRow("e:w");
            outliner.clickFold("e:w");
            outliner.clickRow("b:w|b");
            CHECK(window.selection().primary()->entityId == "w");
            // Ctrl-click adds
            outliner.clickRow("e:n", Qt::ControlModifier);
            CHECK(window.selection().items().size() == 2);

            outliner.setQuery(QStringLiteral("nav1"));
            outliner.refreshNow();
            CHECK(keys() == std::vector<std::string>{"e:n"});
            outliner.setQuery(QStringLiteral("zzz"));
            outliner.refreshNow();
            CHECK(keys().empty());
            outliner.setQuery(QString());
            outliner.refreshNow();

            // the eye: hides without an undo step, a hidden row cannot be selected and says why, hiding drops it from the selection
            window.selection().select(doc::SelRef::entity("n2"));
            const size_t steps = window.history().canUndo() ? 1 : 0;
            outliner.clickEye("e:n2");
            CHECK(window.hideState().keys.count("e:n2") == 1);
            CHECK(window.selection().empty());
            CHECK((window.history().canUndo() ? 1u : 0u) == steps);
            outliner.refreshNow();
            CHECK(outliner.model().rows[static_cast<size_t>(outliner.rowOfKey("e:n2"))].hidden());
            outliner.clickRow("e:n2");
            CHECK(window.selection().empty());
            CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("eye")));
            outliner.clickEye("e:n2");
            CHECK(window.hideState().keys.empty());
            outliner.clickRow("e:n2");
            CHECK(window.selection().primary()->entityId == "n2");
        }

        SUBCASE("details: typed fields edit the entity as undo steps") {
            window.selection().select(doc::SelRef::entity("n"));
            details.refreshNow();
            REQUIRE(details.form().mode == tools::DetailsForm::Mode::Entity);
            auto* name = qobject_cast<QLineEdit*>(details.editor(QStringLiteral("targetname")));
            REQUIRE(name);
            CHECK(name->text() == QStringLiteral("nav1"));
            name->setText(QStringLiteral("door"));
            emit name->editingFinished();
            const auto entity = [&](const std::string& id) -> const core::Entity& { return window.document()->entities[static_cast<size_t>(window.document()->findEntity(id))]->typed; };
            CHECK(entity("n").keyvalues.at("targetname") == "door");
            window.undo();
            CHECK(entity("n").keyvalues.at("targetname") == "nav1");

            details.refreshNow();
            auto* wait = qobject_cast<QLineEdit*>(details.editor(QStringLiteral("Wait Time")));
            REQUIRE(wait);
            wait->setText(QStringLiteral("0.25"));
            emit wait->editingFinished();
            CHECK(entity("n").keyvalues.at("waitTime") == "0.25");

            // the entity picker
            details.refreshNow();
            auto* add = qobject_cast<QComboBox*>(details.editor(QStringLiteral("Next nav point:add")));
            if (!add) add = details.findChild<QComboBox*>(QStringLiteral("row:Next nav point:add"));
            REQUIRE(add);
            const int target = add->findData(QStringLiteral("n2"));
            REQUIRE(target > 0);
            add->setCurrentIndex(target);
            emit add->activated(target);
            REQUIRE(entity("n").refs.count("target"));
            CHECK(entity("n").refs.at("target").paths == std::vector<std::string>{"n2"});
            CHECK(add->findData(QStringLiteral("n")) < 0);   // an entity is not its own target

            // a refused edit puts the field back
            window.selection().select(doc::SelRef::instance("none"));
            details.refreshNow();
            CHECK(details.form().mode == tools::DetailsForm::Mode::Nothing);
        }

        SUBCASE("details: brush entities show the brush; a prefab instance shows its transform") {
            window.selection().select(doc::SelRef::brush("t", "tb"));
            details.refreshNow();
            REQUIRE(details.form().mode == tools::DetailsForm::Mode::Entity);
            CHECK(details.editor(QStringLiteral("operation")));
            CHECK_FALSE(details.editor(QStringLiteral("origin")));
            auto* op = qobject_cast<QComboBox*>(details.editor(QStringLiteral("operation")));
            REQUIRE(op);
            op->setCurrentIndex(op->findData(QStringLiteral("subtract")));
            emit op->activated(op->currentIndex());
            const auto& brushes = window.document()->entities[static_cast<size_t>(window.document()->findEntity("t"))]->brushes;
            CHECK(brushes[0]->brush.op == core::BrushOp::Subtract);
        }

        SUBCASE("place: a click adds the entity, light or prefab where the camera looks and selects it") {
            REQUIRE(waitUntil([&] { return place.entityList()->count() > 0; }));
            CHECK(place.entityList()->count() == 2);   // nav_point and info_player_start; lights are on their own tab
            CHECK(place.lightList()->count() == 6);    // the five modern lights and the classic one
            REQUIRE(waitUntil([&] { return place.prefabList()->count() == 2; }));
            CHECK(place.prefabFiles() == std::vector<std::string>{"prefabs/door.map.json", "prefabs/sub/window.map.json"});
            CHECK(place.prefabList()->item(0)->text() == QStringLiteral("door"));

            const size_t before = window.document()->entities.size();
            emit place.entityList()->itemClicked(place.entityList()->item(0));
            REQUIRE(window.document()->entities.size() == before + 1);
            REQUIRE(window.selection().primary());
            const core::Entity& e = window.document()->entities.back()->typed;
            CHECK(window.selection().primary()->entityId == e.id);
            CHECK(e.classname == "nav_point");
            window.undo();
            CHECK(window.document()->entities.size() == before);

            QListWidgetItem* spot = nullptr;
            for (int i = 0; i < place.lightList()->count(); ++i)
                if (place.lightList()->item(i)->data(Qt::UserRole).toString() == QStringLiteral("light_spot")) spot = place.lightList()->item(i);
            REQUIRE(spot);
            emit place.lightList()->itemClicked(spot);
            CHECK(window.document()->entities.back()->typed.classname == "light_spot");
            CHECK(window.document()->entities.back()->typed.angles.x == 90);   // spot lights start pointing down

            emit place.prefabList()->itemClicked(place.prefabList()->item(1));
            REQUIRE(window.document()->instances.size() == 1);
            CHECK(window.document()->instances[0]->typed.file == "prefabs/sub/window.map.json");
            REQUIRE(window.selection().primary());
            CHECK(window.selection().primary()->isInstance());

        }

        SUBCASE("screenshot of the docks when NATIVE_SCREENSHOT_DIR is set") {
            const QByteArray out = qgetenv("NATIVE_SCREENSHOT_DIR");
            if (out.isEmpty()) return;
            window.selection().select(doc::SelRef::entity("n"));
            outliner.clickFold("e:t");
            outliner.clickEye("e:n2");
            details.refreshNow();
            pumpFor(300);
            window.grab().save(QString::fromLocal8Bit(out) + QStringLiteral("/panels.png"));
            window.selection().select(doc::SelRef::brush("t", "tb"));
            details.refreshNow();
            window.place()->tabs()->setCurrentIndex(0);
            pumpFor(300);
            window.grab().save(QString::fromLocal8Bit(out) + QStringLiteral("/panels_brush.png"));
        }

        SUBCASE("place: the shape cards and their parameters drive the box tool") {
            auto* cyl = place.findChild<QToolButton*>(QStringLiteral("shape:cylinder"));
            REQUIRE(cyl);
            cyl->click();
            CHECK(window.shape() == tools::ShapeId::Cylinder);
            CHECK(window.tool() == ui::MainWindow::Tool::Box);
            auto* sides = place.findChild<QSpinBox*>(QStringLiteral("param:sides"));
            REQUIRE(sides);
            CHECK(sides->isVisibleTo(&place));
            sides->setValue(20);
            CHECK(window.shapeParams().sides == 20);
            auto* turn = place.findChild<QComboBox*>(QStringLiteral("param:turn"));
            REQUIRE(turn);
            turn->setCurrentIndex(2);
            emit turn->activated(2);
            CHECK(window.shapeParams().turn == 2);
            window.setShape(tools::ShapeId::Stairs);
            place.syncShape();
            CHECK_FALSE(place.findChild<QSpinBox*>(QStringLiteral("param:sides"))->isVisibleTo(&place));
            CHECK(place.findChild<QSpinBox*>(QStringLiteral("param:steps"))->isVisibleTo(&place));
        }
    }
}
