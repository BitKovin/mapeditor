// M11 item 11, U15: theme and interface scale: the stored appearance, applying it to the application, the Appearance section of Settings > General.
#include <doctest.h>

#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include "ui/theme.h"

#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>

using namespace native;

namespace {

// Puts the application back the way it was after a test.
struct Restore {
    ~Restore() { ui::applyAppearance({}); }
};

ui::Services services(QSettings* settings = nullptr) {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.settings = settings;
    return sv;
}

} // namespace

TEST_CASE("theme: the stored appearance is read with limits and defaults") {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    CHECK(ui::readAppearance(&settings) == ui::Appearance{});
    CHECK(ui::readAppearance(nullptr) == ui::Appearance{});
    ui::writeAppearance(&settings, {"dark", 125});
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"dark", 125});
    ui::writeAppearance(&settings, {"purple", 900});
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"system", ui::kScaleMax});
    ui::writeAppearance(&settings, {"light", 3});
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"light", ui::kScaleMin});
}

TEST_CASE("theme: dark and light change the palette, the scale multiplies the font, system and 100 % bring the start back") {
    Restore restore;
    const QFont startFont = QApplication::font();
    const QColor startWindow = QApplication::palette().color(QPalette::Window);
    ui::applyAppearance({"system", 100});   // remembers the start
    ui::applyAppearance({"dark", 100});
    CHECK(QApplication::palette().color(QPalette::Window).lightness() < 80);
    CHECK(QApplication::palette().color(QPalette::Text).lightness() > 180);
    ui::applyAppearance({"light", 100});
    CHECK(QApplication::palette().color(QPalette::Window).lightness() > 180);
    ui::applyAppearance({"system", 150});
    if (startFont.pointSizeF() > 0) CHECK(QApplication::font().pointSizeF() == doctest::Approx(startFont.pointSizeF() * 1.5));
    ui::applyAppearance({"system", 100});
    CHECK(QApplication::font().pointSizeF() == doctest::Approx(startFont.pointSizeF()));
    CHECK(QApplication::palette().color(QPalette::Window) == startWindow);
}

TEST_CASE("theme: the General page of the Settings dialog applies it and stores it") {
    Restore restore;
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    ui::MainWindow window(services(&settings));
    ui::SettingsDialog dialog(window);
    auto* theme = dialog.findChild<QComboBox*>("settings:theme");
    auto* scale = dialog.findChild<QComboBox*>("settings:uiScale");
    REQUIRE(theme);
    REQUIRE(scale);
    CHECK(theme->currentData().toString() == "system");
    CHECK(scale->currentData().toInt() == 100);
    CHECK_FALSE(dialog.dirty());
    theme->setCurrentIndex(theme->findData("dark"));
    scale->setCurrentIndex(scale->findData(125));
    CHECK(dialog.dirty());
    CHECK(dialog.applyAll().isEmpty());
    CHECK_FALSE(dialog.dirty());
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"dark", 125});
    CHECK(QApplication::palette().color(QPalette::Window).lightness() < 80);
}
