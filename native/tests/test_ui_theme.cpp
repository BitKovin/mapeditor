// M11 item 11, U15: theme and interface scale: the stored appearance, applying it to the application, the Appearance section of Settings > General.
#include <doctest.h>

#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include "ui/theme.h"

#include <DockManager.h>

#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>

using namespace native;

namespace {

// Puts the application back the way it was after a test.
struct Restore {
    ~Restore() { ui::applyAppearance({"system", 100}); }
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
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"dark", ui::kScaleMax});
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
    CHECK(theme->currentData().toString() == "dark");
    CHECK(scale->currentData().toInt() == 100);
    CHECK_FALSE(dialog.dirty());
    theme->setCurrentIndex(theme->findData("light"));
    scale->setCurrentIndex(scale->findData(125));
    CHECK(dialog.dirty());
    CHECK(dialog.applyAll().isEmpty());
    CHECK_FALSE(dialog.dirty());
    CHECK(ui::readAppearance(&settings) == ui::Appearance{"light", 125});
    CHECK(QApplication::palette().color(QPalette::Window).lightness() > 180);
}

TEST_CASE("theme: the style sheets carry the tokens, the docking chrome always and the widget rules for dark and light") {
    const ui::Tokens t = ui::darkTokens();
    const QString dock = ui::dockStyleSheet(t);
    CHECK(dock.contains(t.accent.name()));
    CHECK(dock.contains(t.line.name()));
    CHECK_FALSE(dock.contains('@'));   // every placeholder was filled
    CHECK_FALSE(dock.contains("QPushButton"));
    const QString full = ui::appStyleSheet(t, true);
    CHECK(full.contains("QToolBar"));
    CHECK_FALSE(full.contains('@'));
    CHECK(ui::appStyleSheet(ui::lightTokens(), true).contains(ui::lightTokens().bg1.name()));
    // the dark surfaces get lighter step by step, the accent stays a blue
    CHECK(t.bg0.lightness() < t.bg1.lightness());
    CHECK(t.bg1.lightness() < t.bg2.lightness());
    CHECK(t.bg3.lightness() < t.bg4.lightness());
    CHECK(t.accent.blue() > t.accent.red());
}

TEST_CASE("theme: applying a theme sets the application style sheet and the current tokens") {
    Restore restore;
    ui::applyAppearance({"dark", 100});
    CHECK(qApp->styleSheet().contains("ads--CDockWidgetTab"));
    CHECK(qApp->styleSheet().contains("QToolBar"));
    CHECK(ui::tokens().bg1 == ui::darkTokens().bg1);
    ui::applyAppearance({"light", 100});
    CHECK(ui::tokens().bg1 == ui::lightTokens().bg1);
    ui::applyAppearance({"system", 100});
    CHECK(qApp->styleSheet().contains("ads--CDockWidgetTab"));
    CHECK_FALSE(qApp->styleSheet().contains("QToolBar"));   // the system's own look keeps its widgets
}

TEST_CASE("theme: the docking chrome is quiet (no close, float or tab-list buttons on an area, the docking system's own style sheet is off)") {
    ui::MainWindow window(services());
    CHECK(ads::CDockManager::testConfigFlag(ads::CDockManager::DisableStylesheet));
    CHECK_FALSE(ads::CDockManager::testConfigFlag(ads::CDockManager::DockAreaHasCloseButton));
    CHECK_FALSE(ads::CDockManager::testConfigFlag(ads::CDockManager::DockAreaHasUndockButton));
    CHECK_FALSE(ads::CDockManager::testConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton));
    CHECK_FALSE(ads::CDockManager::testConfigFlag(ads::CDockManager::ActiveTabHasCloseButton));
}
