// T11.4a: the Texture Properties window (offscreen, never shown): the fields edit the sidecar JSON, the write is debounced and happens on flush, the hotspot canvas draws, moves and
// removes rectangles. The store is a fake (no assets folder).
#include <doctest.h>

#include <QTest>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

#include <map>

#include "doc/io.h"
#include "tools/texturing/props_edit.h"
#include "ui/app_actions.h"
#include "ui/main_window.h"
#include "ui/texture_props_window.h"

using namespace native;
namespace tex = native::tools::tex;
using json = nlohmann::json;

namespace {

ui::Services services() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

struct Fixture {
    ui::MainWindow window;
    std::map<std::string, json> disk;
    int writes = 0;
    Fixture() : window(services()) {
        window.setTexturePropsStore([this](const std::string& t) { return disk.count(t) ? disk[t] : json::object(); },
                                    [this](const std::string& t, const json& v) {
                                        disk[t] = v;
                                        ++writes;
                                        return std::string();
                                    });
    }
};

template <class T>
T* part(ui::TexturePropsDialog* d, const char* name) {
    return static_cast<T*>(d->part(QString::fromLatin1(name)));
}

} // namespace

TEST_CASE("texture properties: the fields edit the sidecar, the write waits for flush and replaces the stored keys") {
    Fixture f;
    f.disk["wall/brick"] = json{{"pixelsPerMeter", 32}, {"hotspots", json::array({json{{"x", 0.1}, {"y", 0.1}, {"w", 0.3}, {"h", 0.3}}})}};
    ui::TexturePropsDialog* d = f.window.openTextureProperties("wall/brick", false);
    REQUIRE(d);
    CHECK(part<QDoubleSpinBox>(d, "ppm")->value() == doctest::Approx(32));
    CHECK(part<QListWidget>(d, "hotspots")->count() == 1);
    CHECK_FALSE(d->dirty());
    // the fields
    part<QDoubleSpinBox>(d, "ppm")->setValue(128);
    CHECK(d->dirty());
    CHECK(tex::ppmOf(d->props()) == doctest::Approx(128));
    part<QComboBox>(d, "modeV")->setCurrentIndex(2);   // anchor
    part<QDoubleSpinBox>(d, "splitV")->setValue(0.25);
    part<QComboBox>(d, "edgeU")->setCurrentIndex(2);   // clamp
    const tex::AxisProps v = tex::axisOf(d->props(), 1);
    CHECK(v.mode == "anchor");
    CHECK(v.split == doctest::Approx(0.25));
    CHECK(tex::axisOf(d->props(), 0).edge == "clamp");
    part<QComboBox>(d, "transMode")->setCurrentIndex(2);   // cutout
    CHECK(tex::transparencyOf(d->props()).mode == "cutout");
    CHECK(f.writes == 0);   // debounced
    CHECK(d->flush().empty());
    CHECK(f.writes == 1);
    CHECK_FALSE(d->dirty());
    CHECK(f.disk["wall/brick"]["pixelsPerMeter"].get<double>() == doctest::Approx(128));
    CHECK(f.disk["wall/brick"]["repeat"]["v"]["mode"] == "anchor");
    // default ppm: the key is gone from the value that is written
    part<QDoubleSpinBox>(d, "ppm")->setValue(0);
    d->flush();
    CHECK_FALSE(f.disk["wall/brick"].contains("pixelsPerMeter"));
    // a second flush with nothing changed writes nothing
    const int writes = f.writes;
    d->flush();
    CHECK(f.writes == writes);
    // revert goes back to what was on disk when the window opened
    static_cast<QPushButton*>(d->part("btn:revert"))->click();
    CHECK(tex::ppmOf(d->props()) == doctest::Approx(32));
    CHECK(tex::axisOf(d->props(), 1) == tex::defaultAxis(1));
    d->flush();
    CHECK(f.disk["wall/brick"]["pixelsPerMeter"].get<double>() == doctest::Approx(32));
    CHECK(f.disk["wall/brick"]["repeat"].empty());
}

TEST_CASE("texture properties: the opacity of a translucent texture can be changed and is written (cutoff only for cutout)") {
    Fixture f;
    f.disk["glass/pane"] = json::object();
    ui::TexturePropsDialog* d = f.window.openTextureProperties("glass/pane", false);
    REQUIRE(d);
    auto* mode = part<QComboBox>(d, "transMode");
    auto* cutoff = part<QDoubleSpinBox>(d, "cutoff");
    auto* opacity = part<QDoubleSpinBox>(d, "opacity");
    // Auto / Opaque: neither field applies
    CHECK_FALSE(cutoff->isEnabled());
    CHECK_FALSE(opacity->isEnabled());
    // choosing Translucent by hand enables the opacity (the bug: it stayed greyed out until the window was opened again)
    mode->setCurrentIndex(3);
    CHECK(opacity->isEnabled());
    CHECK_FALSE(cutoff->isEnabled());
    opacity->setValue(0.4);
    CHECK(tex::transparencyOf(d->props()).mode == "translucent");
    CHECK(tex::transparencyOf(d->props()).opacity == doctest::Approx(0.4));
    CHECK(d->flush().empty());
    CHECK(f.disk["glass/pane"]["transparency"]["mode"] == "translucent");
    CHECK(f.disk["glass/pane"]["transparency"]["opacity"].get<double>() == doctest::Approx(0.4));
    // Cutout swaps the enabled field
    mode->setCurrentIndex(2);
    CHECK(cutoff->isEnabled());
    CHECK_FALSE(opacity->isEnabled());
    cutoff->setValue(0.3);
    CHECK(tex::transparencyOf(d->props()).cutoff == doctest::Approx(0.3));
    // typing into the field works too (the keyboard path of a spin box)
    mode->setCurrentIndex(3);
    auto* edit = opacity->findChild<QLineEdit*>();
    REQUIRE(edit);
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("0") + opacity->locale().decimalPoint() + QStringLiteral("75"));
    QTest::keyClick(edit, Qt::Key_Return);
    CHECK(tex::transparencyOf(d->props()).opacity == doctest::Approx(0.75).epsilon(0.01));
    // a window opened on a sidecar that is translucent starts with the opacity enabled
    d->flush();
    ui::TexturePropsDialog* e = f.window.openTextureProperties("glass/pane", false);
    REQUIRE(e);
    CHECK(part<QDoubleSpinBox>(e, "opacity")->isEnabled() == (tex::transparencyOf(e->props()).mode == "translucent"));
}

TEST_CASE("texture properties: the hotspot canvas draws, selects, moves and removes rectangles; the list and the numbers follow") {
    Fixture f;
    ui::TexturePropsDialog* d = f.window.openTextureProperties("wall/tiles", false);
    REQUIRE(d);
    ui::HotspotCanvas* cv = d->canvas();
    cv->resize(300, 300);
    CHECK(cv->hotspots().empty());
    // draw one: drag from 20% to 60%
    cv->press(cv->toPixel({0.2, 0.2}));
    cv->move(cv->toPixel({0.4, 0.4}));
    cv->move(cv->toPixel({0.6, 0.6}));
    cv->release();
    REQUIRE(cv->hotspots().size() == 1);
    CHECK(cv->hotspots()[0].x == doctest::Approx(0.2).epsilon(0.02));
    CHECK(cv->hotspots()[0].w == doctest::Approx(0.4).epsilon(0.02));
    CHECK(cv->selected() == 0);
    CHECK(tex::hotspotsOf(d->props()).size() == 1);
    CHECK(part<QListWidget>(d, "hotspots")->count() == 1);
    CHECK(part<QDoubleSpinBox>(d, "hx")->value() == doctest::Approx(0.2).epsilon(0.02));
    // a click on empty space only deselects; a click inside selects; a drag inside moves
    cv->press(cv->toPixel({0.9, 0.9}));
    cv->release();
    CHECK(cv->selected() == -1);
    CHECK(cv->hotspots().size() == 1);
    cv->press(cv->toPixel({0.4, 0.4}));
    cv->move(cv->toPixel({0.5, 0.4}));
    cv->release();
    CHECK(cv->selected() == 0);
    CHECK(cv->hotspots()[0].x == doctest::Approx(0.3).epsilon(0.05));
    // the numbers edit the selected rectangle
    part<QDoubleSpinBox>(d, "hw")->setValue(0.2);
    CHECK(cv->hotspots()[0].w == doctest::Approx(0.2).epsilon(0.02));
    // add / remove
    static_cast<QPushButton*>(d->part("btn:addHotspot"))->click();
    CHECK(cv->hotspots().size() == 2);
    CHECK(cv->selected() == 1);
    static_cast<QPushButton*>(d->part("btn:removeHotspot"))->click();
    CHECK(cv->hotspots().size() == 1);
    CHECK(tex::hotspotsOf(d->props()).size() == 1);
    d->flush();
    CHECK(f.disk["wall/tiles"]["hotspots"].size() == 1);
}

TEST_CASE("texture properties: opening the same texture again reuses the window; another one replaces it after writing the changes") {
    Fixture f;
    auto* a = f.window.openTextureProperties("a/one", false);
    REQUIRE(a);
    CHECK(f.window.openTextureProperties("a/one", false) == a);
    part<QDoubleSpinBox>(a, "ppm")->setValue(16);
    auto* b = f.window.openTextureProperties("a/two", false);
    REQUIRE(b);
    CHECK(b->texture() == "a/two");
    CHECK(f.disk["a/one"]["pixelsPerMeter"].get<double>() == doctest::Approx(16));   // written when the window was replaced
}
