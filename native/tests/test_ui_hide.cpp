// T11.10e: the Hide dialog: rules, the classes of the map, adding and removing patterns, Show all classes.
#include <doctest.h>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "doc/edit.h"
#include "doc/io.h"
#include "tools/visibility.h"
#include "ui/app_actions.h"
#include "ui/hide_dialog.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> mapWithVolumes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {4, 2, 3}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    int n = 0;
    for (const char* cls : {"trigger_once", "trigger_hurt", "func_door", "func_door"}) {
        core::Entity e;
        e.id = "e" + std::to_string(++n);
        e.classname = cls;
        e.brushes.push_back(core::Brush::makeBox("b", {10, 0, 0}, {12, 2, 3}, "dev/grey"));
        REQUIRE(ed.addEntity(e));
    }
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

QCheckBox* box(ui::HideDialog& d, const QString& name) { return qobject_cast<QCheckBox*>(d.part(name)); }

} // namespace

TEST_CASE("hide dialog: the preset rules and the classes of the map are listed") {
    ui::MainWindow window(services());
    window.setDocument(mapWithVolumes(), true);
    ui::HideDialog* d = window.openHideDialog(false);
    REQUIRE(d);
    REQUIRE(box(*d, "rule:0"));
    CHECK(box(*d, "rule:0")->text() == "trigger_*");
    CHECK(box(*d, "rule:1")->text() == "editor_playable_area");
    CHECK_FALSE(box(*d, "rule:0")->isChecked());
    CHECK(d->part("remove:0") == nullptr);   // a preset is only switched
    REQUIRE(box(*d, "class:trigger_once"));
    CHECK(box(*d, "class:func_door")->text().contains("2"));   // two of them
    CHECK(d->part("class:worldspawn") == nullptr);
    CHECK(d->part("empty") == nullptr);
    CHECK(qobject_cast<QLabel*>(d->part("summary"))->text() == "No rule is on");
    CHECK_FALSE(d->part("showAll")->isEnabled());
}

TEST_CASE("hide dialog: a rule box hides the class, the class list and the summary follow") {
    ui::MainWindow window(services());
    window.setDocument(mapWithVolumes(), true);
    ui::HideDialog* d = window.openHideDialog(false);
    box(*d, "rule:0")->setChecked(true);
    CHECK(window.hideState().rules[0].enabled);
    CHECK(window.hideState().entityHidden("e1", "trigger_once"));
    CHECK_FALSE(window.hideState().entityHidden("e3", "func_door"));
    // the dialog was refilled: trigger_once is hidden by the wider rule, so its own box is ticked and not for the user to switch
    REQUIRE(box(*d, "class:trigger_once"));
    CHECK(box(*d, "class:trigger_once")->isChecked());
    CHECK_FALSE(box(*d, "class:trigger_once")->isEnabled());
    CHECK(box(*d, "class:func_door")->isEnabled());
    CHECK(qobject_cast<QLabel*>(d->part("summary"))->text() == "1 rule on");
    CHECK(d->part("showAll")->isEnabled());
}

TEST_CASE("hide dialog: a class box adds a rule of that class, unticking removes it again") {
    ui::MainWindow window(services());
    window.setDocument(mapWithVolumes(), true);
    ui::HideDialog* d = window.openHideDialog(false);
    box(*d, "class:func_door")->setChecked(true);
    REQUIRE(window.hideState().rules.size() == 3);
    CHECK(window.hideState().rules[2].pattern == "func_door");
    CHECK(window.hideState().rules[2].custom);
    CHECK(window.hideState().entityHidden("e3", "func_door"));
    REQUIRE(d->part("remove:2"));   // a rule of the user's can be removed
    box(*d, "class:func_door")->setChecked(false);
    CHECK(window.hideState().rules.size() == 2);
    CHECK_FALSE(window.hideState().entityHidden("e3", "func_door"));
}

TEST_CASE("hide dialog: patterns are added from the field, junk is ignored, a custom rule goes with its button") {
    ui::MainWindow window(services());
    window.setDocument(mapWithVolumes(), true);
    ui::HideDialog* d = window.openHideDialog(false);
    CHECK_FALSE(d->addPattern("   "));
    CHECK(window.hideState().rules.size() == 2);
    qobject_cast<QLineEdit*>(d->part("input"))->setText("Func_*");
    qobject_cast<QPushButton*>(d->part("add"))->click();
    REQUIRE(window.hideState().rules.size() == 3);
    CHECK(window.hideState().rules[2].pattern == "func_*");   // normalised (lower case)
    CHECK(window.hideState().rules[2].enabled);
    CHECK(qobject_cast<QLineEdit*>(d->part("input"))->text().isEmpty());
    CHECK(window.hideState().entityHidden("e3", "func_door"));
    // the same pattern again does not duplicate it
    CHECK(d->addPattern("FUNC_*"));
    CHECK(window.hideState().rules.size() == 3);
    qobject_cast<QPushButton*>(d->part("remove:2"))->click();
    CHECK(window.hideState().rules.size() == 2);
    CHECK_FALSE(window.hideState().entityHidden("e3", "func_door"));
}

TEST_CASE("hide dialog: Show all classes switches every rule off and keeps the rules") {
    ui::MainWindow window(services());
    window.setDocument(mapWithVolumes(), true);
    ui::HideDialog* d = window.openHideDialog(false);
    d->addPattern("func_*");
    d->setRuleEnabled(0, true);
    REQUIRE(window.hideState().any());
    qobject_cast<QPushButton*>(d->part("showAll"))->click();
    CHECK_FALSE(window.hideState().any());
    CHECK(window.hideState().rules.size() == 3);   // the custom rule stays, off
    CHECK_FALSE(box(*d, "rule:2")->isChecked());
    CHECK(qobject_cast<QLabel*>(d->part("summary"))->text() == "No rule is on");
}

TEST_CASE("hide dialog: the View > Hide menu opens it and a map without volumes says so") {
    ui::MainWindow window(services());
    REQUIRE(window.registry().find("view.hide_dialog") != nullptr);
    CHECK(window.hideDialog() == nullptr);
    window.registry().find("view.hide_dialog");
    ui::HideDialog* d = window.openHideDialog(false);
    CHECK(d->part("empty") != nullptr);
    window.setDocument(mapWithVolumes(), true);   // the open dialog follows the document
    CHECK(d->part("empty") == nullptr);
    CHECK(box(*d, "class:trigger_hurt") != nullptr);
}
