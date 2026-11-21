// T11.10b: the Level dock (offscreen, never shown): fields show the worldspawn keys, edits are undo steps, resets, ambient, sky.
#include <doctest.h>

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

#include "doc/edit.h"
#include "doc/io.h"
#include "doc/level_settings.h"
#include "ui/app_actions.h"
#include "ui/level_settings_panel.h"
#include "ui/main_window.h"
#include "ui/texture_browser_panel.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> mapWith(const std::vector<std::pair<std::string, std::string>>& keys) {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {4, 2, 3}, "dev/grey"));
    for (const auto& [k, v] : keys) w.keyvalues[k] = v;
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

std::string key(ui::MainWindow& w, const std::string& k) {
    const core::Entity* world = doc::worldspawnOf(*w.document());
    const auto it = world->keyvalues.find(k);
    return it == world->keyvalues.end() ? std::string("<none>") : it->second;
}

QLineEdit* field(ui::LevelSettingsPanel* p, const char* k) { return static_cast<QLineEdit*>(p->part(QString("field:") + k)); }

} // namespace

TEST_CASE("level panel: the dock, its action and the fields filled from the worldspawn") {
    ui::MainWindow window(services());
    window.setDocument(mapWith({{"_ao_radius", "2.5"}, {"_ambient", "0.2 0.1 0.05"}, {"_sky_auto", "box"}, {"_sky_margin", "20"}, {"_sky_texture", "env/skycube"}}), true);
    ui::LevelSettingsPanel* p = window.levelPanel();
    REQUIRE(p);
    CHECK(window.levelDock());
    CHECK(window.registry().find("view.level.toggle"));
    p->refreshNow();
    CHECK(field(p, "_ao_radius")->text() == "2.5");
    CHECK(field(p, "_ao_power")->text().isEmpty());
    CHECK(field(p, "_ao_power")->placeholderText() == "1");
    CHECK(field(p, "_cc_exposure")->placeholderText() == "lighting.json");
    CHECK(static_cast<QLineEdit*>(p->part("ambientIntensity"))->text() == "0.2");
    CHECK(static_cast<QComboBox*>(p->part("skyAuto"))->currentText() == "Box");
    CHECK(static_cast<QLineEdit*>(p->part("skyMargin"))->text() == "20");
    CHECK(static_cast<QLineEdit*>(p->part("skyTexture"))->text() == "env/skycube");
    CHECK(static_cast<QPushButton*>(p->part("reset:_ao_radius"))->isEnabled());
    CHECK_FALSE(static_cast<QPushButton*>(p->part("reset:_ao_power"))->isEnabled());
}

TEST_CASE("level panel: a typed value is one undo step; the default and empty remove the key") {
    ui::MainWindow window(services());
    window.setDocument(mapWith({}), true);
    ui::LevelSettingsPanel* p = window.levelPanel();
    CHECK(p->setValue("_ao_radius", "3"));
    CHECK(key(window, "_ao_radius") == "3");
    CHECK(window.canUndo());
    CHECK(p->setValue("_ao_rays", "100.4"));
    CHECK(key(window, "_ao_rays") == "100");
    CHECK(p->setValue("_light_scale", "500"));
    CHECK(key(window, "_light_scale") == "100");   // clamped
    CHECK(p->setValue("_light_scale", "1"));        // the default: the key goes
    CHECK(key(window, "_light_scale") == "<none>");
    CHECK_FALSE(p->setValue("_light_scale", ""));   // already absent: nothing changes
    CHECK_FALSE(p->setValue("_not_a_field", "1"));
    window.undo();
    CHECK(key(window, "_light_scale") == "100");
    window.undo();
    window.undo();
    CHECK(key(window, "_ao_radius") == "3");
    window.undo();
    CHECK(key(window, "_ao_radius") == "<none>");
}

TEST_CASE("level panel: editing a field and the reset button go through the widgets") {
    ui::MainWindow window(services());
    window.setDocument(mapWith({}), true);
    ui::LevelSettingsPanel* p = window.levelPanel();
    QLineEdit* e = field(p, "_ao_power");
    e->setText("2.5");
    emit e->editingFinished();
    CHECK(key(window, "_ao_power") == "2.5");
    p->refreshNow();
    auto* reset = static_cast<QPushButton*>(p->part("reset:_ao_power"));
    REQUIRE(reset->isEnabled());
    reset->click();
    CHECK(key(window, "_ao_power") == "<none>");
    p->refreshNow();
    CHECK(e->text().isEmpty());
    CHECK_FALSE(reset->isEnabled());
}

TEST_CASE("level panel: the ambient light") {
    ui::MainWindow window(services());
    window.setDocument(mapWith({}), true);
    ui::LevelSettingsPanel* p = window.levelPanel();
    CHECK(p->setAmbient({1, 0.5, 0.25}, 0.4));
    CHECK(key(window, "_ambient") == "0.4 0.2 0.1");
    p->refreshNow();
    CHECK(static_cast<QLineEdit*>(p->part("ambientIntensity"))->text() == "0.4");
    CHECK(p->setAmbient({1, 0.5, 0.25}, 0));
    CHECK(key(window, "_ambient") == "<none>");
}

TEST_CASE("level panel: sky settings; Auto sky takes the first skybox of env/ in the same step") {
    ui::MainWindow window(services());
    window.setDocument(mapWith({}), true);
    window.textureBrowser()->setTextures({"dev/grey", "env/zcube", "env/acube", "env/notasky"});
    ui::LevelSettingsPanel* p = window.levelPanel();
    const size_t before = window.undoText().size();
    (void)before;
    CHECK(p->setAutoSky(doc::AutoSky::Areas));
    CHECK(key(window, "_sky_auto") == "areas");
    CHECK(key(window, "_sky_texture") == "env/acube");
    window.undo();   // one step for both keys
    CHECK(key(window, "_sky_auto") == "<none>");
    CHECK(key(window, "_sky_texture") == "<none>");
    window.redo();
    CHECK(p->setSkyTexture("env/zcube"));
    CHECK(p->setAutoSky(doc::AutoSky::Box));   // the level has a skybox now: it stays
    CHECK(key(window, "_sky_texture") == "env/zcube");
    CHECK(key(window, "_sky_auto") == "box");
    CHECK(p->setSkyMargin("12"));
    CHECK(key(window, "_sky_margin") == "12");
    CHECK(p->setSkyMargin("8"));   // the default
    CHECK(key(window, "_sky_margin") == "<none>");
    CHECK(p->setAutoSky(doc::AutoSky::Off));
    CHECK(key(window, "_sky_auto") == "<none>");
    CHECK(p->setSkyTexture(""));
    CHECK(key(window, "_sky_texture") == "<none>");
    p->refreshNow();
    p->setSkyTexture("textures/plain");
    p->refreshNow();
    CHECK_FALSE(p->part("skyWarn")->isHidden());
}

TEST_CASE("level panel: a map without a worldspawn gets one when a value is set") {
    ui::MainWindow window(services());
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    window.setDocument(r.version, true);
    ui::LevelSettingsPanel* p = window.levelPanel();
    CHECK(p->setValue("_ao_radius", "4"));
    CHECK(key(window, "_ao_radius") == "4");
}
