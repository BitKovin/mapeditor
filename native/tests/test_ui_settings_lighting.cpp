// T11.10d: the Lighting page of the Settings dialog: the stored values, only changed keys written, reset to default, nothing without a game folder.
#include <doctest.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QToolButton>

#include <QPixmap>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "tools/lighting_config.h"
#include "ui/main_window.h"
#include "ui/settings_dialog.h"

using namespace native;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Game {
    fs::path root;
    Game() {
        root = fs::temp_directory_path() / ("settings_light_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Game() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path file() const { return root / "lighting.json"; }
    void put(const std::string& text) const { std::ofstream(file(), std::ios::binary) << text; }
    json get() const {
        std::ifstream in(file(), std::ios::binary);
        return json::parse(in);
    }
};

ui::Services services(const std::string& assets) {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = assets;
    return sv;
}

template <class T>
T* edit(ui::SettingsDialog& d, const char* key) {
    return d.findChild<T*>(QString("settings:lighting:") + key);
}

} // namespace

TEST_CASE("settings lighting: the page shows the stored values and Keymap moved to the end") {
    Game g;
    g.put(R"({"texelsPerMeter": 16, "useGpu": false, "denoiseQuality": 2})");
    ui::MainWindow window(services(g.root.generic_string()));
    ui::SettingsDialog dialog(window);
    CHECK(ui::SettingsDialog::Lighting == 3);
    CHECK(ui::SettingsDialog::Keymap == 4);
    CHECK(edit<QDoubleSpinBox>(dialog, "texelsPerMeter")->value() == 16);
    CHECK(edit<QCheckBox>(dialog, "useGpu")->isChecked() == false);
    CHECK(edit<QComboBox>(dialog, "denoiseQuality")->currentData().toInt() == 2);
    // a key that is not in the file shows the default
    const tools::LightingConfig c = tools::readLightingConfig(g.file());
    CHECK(edit<QDoubleSpinBox>(dialog, "atlasSize")->value() == c.defaults["atlasSize"].get<double>());
    CHECK_FALSE(dialog.dirty());
    CHECK(dialog.findChild<QLabel*>("settings:lightingNote")->text().contains("lighting.json"));
}

TEST_CASE("settings lighting: every key of the Core has an editor") {
    Game g;
    ui::MainWindow window(services(g.root.generic_string()));
    ui::SettingsDialog dialog(window);
    const tools::LightingConfig c = tools::readLightingConfig(g.file());
    for (const auto& [key, value] : c.values.items()) {
        INFO("key " << key);
        CHECK((edit<QCheckBox>(dialog, key.c_str()) || edit<QComboBox>(dialog, key.c_str()) || edit<QDoubleSpinBox>(dialog, key.c_str())));
    }
}

TEST_CASE("settings lighting: apply writes only the changed keys and keeps the rest of the file") {
    Game g;
    g.put(R"({"texelsPerMeter": 16, "myOwnNote": "keep me"})");
    ui::MainWindow window(services(g.root.generic_string()));
    ui::SettingsDialog dialog(window);
    edit<QDoubleSpinBox>(dialog, "maxPasses")->setValue(7);
    edit<QCheckBox>(dialog, "useGpu")->setChecked(!edit<QCheckBox>(dialog, "useGpu")->isChecked());
    edit<QComboBox>(dialog, "denoiseQuality")->setCurrentIndex(0);
    CHECK(dialog.dirty());
    CHECK(dialog.applyAll().isEmpty());
    const json j = g.get();
    CHECK(j["maxPasses"] == 7);
    CHECK(j["maxPasses"].is_number_integer());
    CHECK(j["texelsPerMeter"] == 16);   // untouched keys are not rewritten
    CHECK(j["myOwnNote"] == "keep me");
    CHECK_FALSE(j.contains("atlasSize"));
    CHECK_FALSE(dialog.dirty());   // the page now shows the stored values
    // the Core reads it back
    CHECK(tools::readLightingConfig(g.file()).values["maxPasses"] == 7);
    // nothing changed: nothing written
    const auto before = fs::last_write_time(g.file());
    CHECK(dialog.applyAll().isEmpty());
    CHECK(fs::last_write_time(g.file()) == before);
}

TEST_CASE("settings lighting: Reset puts the default back and a fractional key stays fractional") {
    Game g;
    g.put(R"({"texelsPerMeter": 99, "exposure": 2.5})");
    ui::MainWindow window(services(g.root.generic_string()));
    ui::SettingsDialog dialog(window);
    const tools::LightingConfig c = tools::readLightingConfig(g.file());
    dialog.findChild<QToolButton*>("settings:lighting:reset:texelsPerMeter")->click();
    CHECK(edit<QDoubleSpinBox>(dialog, "texelsPerMeter")->value() == c.defaults["texelsPerMeter"].get<double>());
    CHECK(dialog.dirty());
    edit<QDoubleSpinBox>(dialog, "exposure")->setValue(1.25);
    CHECK(dialog.applyAll().isEmpty());
    const json j = g.get();
    CHECK(j["exposure"] == 1.25);
    CHECK(j["texelsPerMeter"] == c.defaults["texelsPerMeter"]);
}

TEST_CASE("settings lighting: without a game folder the page is off and nothing is written") {
    ui::MainWindow window(services(""));
    ui::SettingsDialog dialog(window);
    CHECK_FALSE(dialog.findChild<QWidget*>("settings:lightingNote")->parentWidget()->isEnabled());
    CHECK_FALSE(dialog.dirty());
    CHECK(dialog.applyAll().isEmpty());
}

// A picture of the page for a look by eye: NATIVE_SHOT=<png path> saves it (an offscreen grab, no window is shown); without it the case does nothing.
TEST_CASE("settings lighting: a picture of the page (NATIVE_SHOT)") {
    const QByteArray out = qgetenv("NATIVE_SHOT");
    if (out.isEmpty()) return;
    Game g;
    g.put(R"({"texelsPerMeter": 16, "useGpu": false, "denoiseQuality": 2})");
    ui::MainWindow window(services(g.root.generic_string()));
    ui::SettingsDialog dialog(window);
    dialog.showPage(ui::SettingsDialog::Lighting);
    dialog.resize(900, 760);
    CHECK(dialog.grab().save(QString::fromLocal8Bit(out)));
}
