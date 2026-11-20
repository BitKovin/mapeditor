// T4.5 (Qt side): the status bar fields and the Settings dialog in a real window (native_gui_tests).
#include <doctest.h>

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QCheckBox>
#include <QTemporaryDir>
#include <QToolButton>

#include <core/profile.h>

#include "base/jobs.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "ui/place_panel.h"
#include "ui/settings_dialog.h"
#include "ui/status_bar.h"

using namespace native;
using namespace guitest;

namespace {

void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

std::shared_ptr<const doc::DocumentVersion> smallMap() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity n;
    n.id = "n";
    n.classname = "nav_point";
    REQUIRE(ed.addEntity(n));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

const char* kEntA = R"(<point name="nav_point" color="0 0.5 0" box="-8 -8 0 8 8 32">Nav point.</point>)";
const char* kEntB = R"(<point name="other_point" color="0.5 0 0" box="-8 -8 0 8 8 32">Other.</point>)";

template <typename W>
W* named(QWidget& root, const char* name) {
    W* w = root.findChild<W*>(QString::fromLatin1(name));
    REQUIRE_MESSAGE(w, name);
    return w;
}

bool hasItem(QListWidget* list, const QString& text) {
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().contains(text)) return true;
    return false;
}

} // namespace

TEST_CASE("status bar fields and the Settings dialog in a window" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir store, gameA, gameB;
    write(gameA.path() + "/entities.ent", kEntA);
    write(gameA.path() + "/prefabs/door.map.json", R"({"schemaVersion": "0.2", "entities": []})");
    write(gameB.path() + "/entities.ent", kEntB);
    write(gameB.path() + "/prefabs/window.map.json", R"({"schemaVersion": "0.2", "entities": []})");
    const QString ini = store.path() + "/settings.ini";

    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = gameA.path().toStdString();
    std::vector<QString> errors;
    {
        QSettings settings(ini, QSettings::IniFormat);
        sv.settings = &settings;
        ui::MainWindow window(sv);
        window.setErrorHandler([&](const QString& m) { errors.push_back(m); });
        window.resize(1100, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(smallMap(), true);
        ui::StatusFields& status = *window.statusFields();
        REQUIRE(window.place());
        pumpFor(100);

        SUBCASE("the status bar follows the tool, the selection, the grid, the map and the folder") {
            status.refreshNow();
            CHECK(status.toolText() == "Tool: Select");
            CHECK(status.selectionText() == "Nothing selected");
            CHECK(status.mapText() == "untitled");
            CHECK(status.folderText() == QFileInfo(gameA.path()).fileName());
            CHECK(status.gridText().startsWith("Grid 0.5 m"));

            window.setTool(ui::MainWindow::Tool::Box);
            window.selection().select(doc::SelRef::entity("n"));
            render::ViewportWindow* vp = window.activeViewport();
            REQUIRE(vp);
            render::ViewportSettings s = vp->rig().settings();
            s.gridSize = 4;
            s.snapEnabled = false;
            vp->rig().applySettings(s);
            vp->viewChanged();
            // the labels follow without anyone asking (coalesced to the next event-loop turn)
            REQUIRE(waitUntil([&] { return status.toolText() == "Tool: Box" && status.selectionText() == "nav_point" && status.gridText() == "Grid 4 m, snap off"; }, 3000));

            // changes that touch nothing shown do not rebuild the labels
            const int before = status.updates();
            window.selection().select(doc::SelRef::entity("n"));
            pumpFor(50);
            CHECK(status.updates() == before);

            // an edit marks the map
            window.selection().select(doc::SelRef::entity("n"));
            REQUIRE(window.duplicateSelected());
            REQUIRE(waitUntil([&] { return status.mapText() == "untitled *"; }, 3000));
            // a click on the folder opens Settings (the button exists and carries the folder)
            CHECK(status.folderButton()->isVisible());
        }

        SUBCASE("changing the game folder reloads the profile, the entity definitions and the Place panel") {
            REQUIRE(window.entityDefinitions());
            CHECK(window.entityDefinitions()->find("nav_point"));
            CHECK(hasItem(window.place()->entityList(), "nav_point"));
            REQUIRE(window.place()->prefabFiles().size() == 1);

            ui::SettingsDialog dialog(window);
            dialog.show();
            CHECK(named<QLineEdit>(dialog, "settings:root")->text() == gameA.path());
            named<QLineEdit>(dialog, "settings:root")->setText(gameB.path());
            CHECK(dialog.dirty());
            CHECK(dialog.applyAll().isEmpty());
            CHECK(window.assetsRoot() == gameB.path().toStdString());
            REQUIRE(window.entityDefinitions());
            CHECK(window.entityDefinitions()->find("other_point"));
            CHECK_FALSE(window.entityDefinitions()->find("nav_point"));
            CHECK(hasItem(window.place()->entityList(), "other_point"));
            CHECK_FALSE(hasItem(window.place()->entityList(), "nav_point"));
            REQUIRE(window.place()->prefabFiles().size() == 1);
            CHECK(window.place()->prefabFiles()[0].find("window") != std::string::npos);
            // it persists
            CHECK(settings.value("assets/root").toString() == gameB.path());
            // the dialog shows the new folder and the status bar follows
            CHECK(named<QLineEdit>(dialog, "settings:root")->text() == gameB.path());
            CHECK_FALSE(dialog.dirty());
            status.refreshNow();
            CHECK(status.folderText() == QFileInfo(gameB.path()).fileName());

            // a folder that does not exist is refused and nothing changes
            named<QLineEdit>(dialog, "settings:root")->setText(gameA.path() + "/no/such/folder");
            const QString err = dialog.applyAll();
            CHECK_FALSE(err.isEmpty());
            CHECK(dialog.error() == err);
            CHECK(window.assetsRoot() == gameB.path().toStdString());
        }

        SUBCASE("a map with unsaved changes asks before the folder is switched") {
            window.selection().select(doc::SelRef::entity("n"));
            REQUIRE(window.duplicateSelected());
            REQUIRE(window.isDirty());
            int asked = 0;
            bool answer = false;
            window.setConfirmHandler([&](const QString&, const QString&) {
                ++asked;
                return answer;
            });
            ui::SettingsDialog dialog(window);
            named<QLineEdit>(dialog, "settings:root")->setText(gameB.path());
            CHECK_FALSE(dialog.applyAll().isEmpty());   // said no
            CHECK(asked == 1);
            CHECK(window.assetsRoot() == gameA.path().toStdString());
            answer = true;
            CHECK(dialog.applyAll().isEmpty());
            CHECK(asked == 2);
            CHECK(window.assetsRoot() == gameB.path().toStdString());
            CHECK(window.isDirty());   // the map stays open and modified
        }

        SUBCASE("game profile page: edits go to game_profile.json and are used at once") {
            ui::SettingsDialog dialog(window);
            dialog.showPage(ui::SettingsDialog::GameProfile);
            auto* maps = named<QLineEdit>(dialog, "settings:profile:mapRoot");
            auto* prefabs = named<QLineEdit>(dialog, "settings:profile:prefabRoot");
            CHECK(maps->text() == "maps");   // the defaults: no file yet
            CHECK(named<QLabel>(dialog, "settings:profileNote")->text().contains("no game_profile.json"));
            CHECK_FALSE(dialog.dirty());
            maps->setText("levels");
            prefabs->setText("prefabs2");
            named<QPlainTextEdit>(dialog, "settings:profile:blockLight")->setPlainText("func_door\nfunc_wall");
            named<QComboBox>(dialog, "settings:profile:exporter")->setCurrentIndex(1);
            CHECK(dialog.dirty());
            CHECK(dialog.applyAll().isEmpty());
            const core::GameProfile p = core::loadProfile(gameA.path().toStdString());
            CHECK(p.mapRoot == "levels");
            CHECK(p.prefabRoot == "prefabs2");
            CHECK(p.exporter == "obj");
            REQUIRE(p.blockLight.size() == 2);
            CHECK(window.prefabRoot() == "prefabs2");   // the window read the profile again
            CHECK_FALSE(dialog.dirty());
            CHECK(named<QLabel>(dialog, "settings:profileNote")->text().contains("Keys not shown here"));
            const QByteArray written = readFile(gameA.path() + "/game_profile.json");

            // a bad value is refused as a whole; the file stays
            maps->setText("../outside");
            CHECK(dialog.applyAll().contains("Maps"));
            CHECK(named<QLabel>(dialog, "settings:error")->isVisibleTo(&dialog));
            CHECK(readFile(gameA.path() + "/game_profile.json") == written);
            // Cancel semantics: a new dialog starts from what is stored
            ui::SettingsDialog again(window);
            CHECK(named<QLineEdit>(again, "settings:profile:mapRoot")->text() == "levels");
        }

        SUBCASE("a broken game_profile.json is reported, not silently replaced") {
            write(gameB.path() + "/game_profile.json", "{ this is not json");
            window.setAssetsRoot(gameB.path().toStdString());
            CHECK_FALSE(window.profileError().empty());
            REQUIRE_FALSE(errors.empty());
            CHECK(errors.back().contains("game_profile.json"));
            ui::SettingsDialog dialog(window);
            CHECK(named<QLabel>(dialog, "settings:rootInfo")->text().contains("broken"));
            CHECK(named<QLabel>(dialog, "settings:profileNote")->text().contains("cannot be read"));
            // fixing the file from the dialog works
            named<QLineEdit>(dialog, "settings:profile:name")->setText("fixed");
            CHECK(dialog.applyAll().isEmpty());
            CHECK(window.profileError().empty());
            CHECK(core::loadProfile(gameB.path().toStdString()).name == "fixed");
        }

        SUBCASE("editor prefs: kept in the settings, used by new viewports") {
            ui::SettingsDialog dialog(window);
            dialog.showPage(ui::SettingsDialog::Editor);
            named<QComboBox>(dialog, "settings:editor:grid")->setCurrentIndex(named<QComboBox>(dialog, "settings:editor:grid")->findData(2.0));
            named<QCheckBox>(dialog, "settings:editor:snap")->setChecked(false);
            named<QDoubleSpinBox>(dialog, "settings:editor:fly")->setValue(30);
            named<QCheckBox>(dialog, "settings:editor:showGrid")->setChecked(false);
            CHECK(dialog.dirty());
            CHECK(dialog.applyAll().isEmpty());
            CHECK(window.editorPrefs().gridSize == 2);
            CHECK_FALSE(window.editorPrefs().snapEnabled);
            CHECK(window.editorPrefs().flySpeed == 30);
            CHECK(settings.value("prefs/editor").toString().contains("gridSize"));
            // open viewports keep their own settings, new ones start from the prefs
            CHECK(window.viewport()->rig().settings().gridSize == 0.5);
            window.setLayoutPreset(ui::LayoutPreset::Two);
            REQUIRE(window.viewports().size() == 2);
            CHECK(window.viewports()[0].window->rig().settings().gridSize == 0.5);
            const render::ViewportSettings added = window.viewports()[1].window->rig().settings();
            CHECK(added.gridSize == 2);
            CHECK_FALSE(added.snapEnabled);
            CHECK(added.flySpeed == 30);
            CHECK_FALSE(added.showGrid);
            // "use the active viewport's values"
            window.activeViewport()->rig().applySettings([&] {
                render::ViewportSettings s = window.activeViewport()->rig().settings();
                s.gridSize = 10;
                return s;
            }());
            named<QPushButton>(dialog, "settings:editor:useViewport")->click();
            CHECK(named<QComboBox>(dialog, "settings:editor:grid")->currentData().toDouble() == 10);
        }

        SUBCASE("screenshots for a look (NATIVE_SCREENSHOT_DIR)") {
            const QByteArray dirName = qgetenv("NATIVE_SCREENSHOT_DIR");
            if (dirName.isEmpty()) return;
            window.selection().select(doc::SelRef::entity("n"));
            pumpFor(100);
            window.grab().save(QString::fromLocal8Bit(dirName) + "/status.png");
            ui::SettingsDialog dialog(window);
            dialog.show();
            for (int i = 0; i < 3; ++i) {
                dialog.showPage(i);
                pumpFor(50);
                dialog.grab().save(QString::fromLocal8Bit(dirName) + QString("/settings%1.png").arg(i));
            }
        }

        SUBCASE("recent maps can be cleared") {
            window.saveDocumentAs("maps/one.map.json");
            REQUIRE(waitUntil([&] { return !window.busy(); }));
            REQUIRE(window.recentFiles().size() == 1);
            ui::SettingsDialog dialog(window);
            CHECK(named<QLabel>(dialog, "settings:editor:recentInfo")->text().contains("1 map"));
            named<QPushButton>(dialog, "settings:editor:clearRecent")->click();
            CHECK(window.recentFiles().empty());
            CHECK(settings.value("files/recent").toStringList().isEmpty());
            CHECK(named<QLabel>(dialog, "settings:editor:recentInfo")->text() == "No recent maps");
        }
        window.saveLayout();
    }
}
