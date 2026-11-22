// M11 item 11, U12: the Lighting dock (list of lights, mute / solo) and the light filter the live lighting applies.
#include <doctest.h>

#include "doc/edit.h"
#include "doc/io.h"
#include "light/light_filter.h"
#include "tools/light_list.h"
#include "ui/lighting_panel.h"
#include "ui/main_window.h"

using namespace native;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

std::shared_ptr<const doc::DocumentVersion> mapWithLights() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("a", {0, 0, 0}, {2, 2, 2}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity l1;
    l1.id = "l1";
    l1.classname = "light_point";
    l1.keyvalues["targetname"] = "lamp";
    l1.keyvalues["brightness"] = "5";
    l1.keyvalues["color"] = "1 0 0";
    REQUIRE(ed.addEntity(l1));
    core::Entity l2;
    l2.id = "l2";
    l2.classname = "light_spot";
    l2.keyvalues["style"] = "3";
    REQUIRE(ed.addEntity(l2));
    core::Entity other;
    other.id = "n";
    other.classname = "nav_point";
    REQUIRE(ed.addEntity(other));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

core::Light lightOf(const std::string& id, const std::string& instance = "") {
    core::Light l;
    l.id = id;
    l.instance = instance;
    return l;
}

} // namespace

TEST_CASE("light list: the light entities of the document with name, class, brightness, colour and style") {
    const auto rows = tools::listLights(*mapWithLights());
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == "l1");
    CHECK(rows[0].name == "lamp");
    CHECK(rows[0].classname == "light_point");
    CHECK(rows[0].brightness == doctest::Approx(5));
    CHECK(rows[0].color == "#ff0000");
    CHECK(rows[1].name == "l2");   // no targetname: the id
    CHECK(rows[1].color == "#ffffff");
    CHECK(rows[1].style == 3);
}

TEST_CASE("light filter: muted lights and a solo are removed from the list the lighting collects; prefab lights have their own keys") {
    light::LightFilter f;
    CHECK_FALSE(f.active());
    std::vector<core::Light> lights = {lightOf("a"), lightOf("b"), lightOf("a", "inst1"), lightOf("c")};
    f.apply(lights);
    CHECK(lights.size() == 4);   // an inactive filter keeps everything
    f.muted = {"b", "inst1/a"};
    CHECK(f.active());
    f.apply(lights);
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].id == "a");
    CHECK(lights[1].id == "c");
    f.solo = "c";
    f.apply(lights);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].id == "c");
    // a solo that is also muted gives nothing
    f.muted.insert("c");
    f.apply(lights);
    CHECK(lights.empty());
    CHECK(light::LightFilter::keyOf("", "x") == "x");
    CHECK(light::LightFilter::keyOf("i/j", "x") == "i/j/x");
    CHECK(light::LightFilter() == light::LightFilter());
}

TEST_CASE("lighting dock: lists the lights, mute and solo change the window's light filter, All lights on clears it") {
    ui::MainWindow window(services());
    ui::LightingPanel* panel = window.lightingPanel();
    REQUIRE(panel);
    CHECK(window.lightFilter() == nullptr);
    window.setDocument(mapWithLights(), true);
    panel->refreshNow();
    CHECK(panel->rows() == QStringList{"lamp | light_point | 5 |  | ", "l2 | light_spot | 1 |  | "});
    CHECK(panel->summary() == QStringLiteral("2 lights"));

    int changed = 0;
    QObject::connect(&window, &ui::MainWindow::lightFilterChanged, [&] { ++changed; });
    panel->setMuteBox(QStringLiteral("l2"), true);
    CHECK(window.lightMuted("l2"));
    CHECK_FALSE(window.lightMuted("l1"));
    REQUIRE(window.lightFilter());
    CHECK(changed == 1);
    panel->refreshNow();
    CHECK(panel->rows()[1] == QStringLiteral("l2 | light_spot | 1 | x | "));
    CHECK(panel->summary().startsWith(QStringLiteral("2 lights, 1 muted")));
    panel->setSoloBox(QStringLiteral("l1"), true);
    CHECK(window.lightSolo() == "l1");
    panel->refreshNow();
    CHECK(panel->summary().contains(QStringLiteral("solo: lamp")));
    // the same state again is no change
    window.setLightSolo("l1");
    CHECK(changed == 2);
    panel->setSoloBox(QStringLiteral("l1"), false);
    CHECK(window.lightSolo().empty());
    window.clearLightFilter();
    CHECK(window.lightFilter() == nullptr);
    panel->refreshNow();
    CHECK(panel->summary() == QStringLiteral("2 lights"));
    CHECK(window.registry().find("view.lighting.toggle") != nullptr);
    CHECK(window.lightingDock() != nullptr);
}
