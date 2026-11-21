// T9.3 (Qt side): File > Export, Play map and Import Quake map through the main window. Offscreen platform, the window is never shown, no GPU.
#include <doctest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>

#include <filesystem>
#include <functional>
#include <fstream>

#include <core/bsp_export.h>

#include "base/jobs.h"
#include "doc/document.h"
#include "ui/main_window.h"

namespace fs = std::filesystem;
using namespace native;

namespace {

bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 10000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return cond();
}

// A scratch game folder: a full copy of the shipped assets (so exporting never writes into the repository).
struct Game {
    QTemporaryDir dir;
    fs::path root;
    QTemporaryDir settingsDir;
    Game() {
        root = fs::path(dir.path().toStdString());
        fs::copy(fs::path(NATIVE_CORE_ASSETS_DIR), root, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
};

struct Errors {
    std::vector<QString> list;
    void attach(ui::MainWindow& w) { w.setErrorHandler([this](const QString& m) { list.push_back(m); }); }
};

} // namespace

TEST_CASE("File > Export writes the map in the game's format on its own thread; Play map needs the game set; the start line is kept per game folder") {
    Game game;
    QSettings settings(QString::fromStdString((fs::path(game.settingsDir.path().toStdString()) / "s.ini").string()), QSettings::IniFormat);
    base::JobPool jobs(2);
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = game.root.generic_string();
    sv.jobs = &jobs;
    sv.settings = &settings;
    ui::MainWindow window(sv);
    Errors errors;
    errors.attach(window);
    window.openDocument("maps/demo_lit.map.json");
    REQUIRE(waitUntil([&] { return window.document() != nullptr && !window.busy(); }, 20000));

    CHECK_FALSE(window.exporting());
    REQUIRE(window.startExport("maps/gui_out.bsp", false, false));
    CHECK(window.exporting());
    CHECK_FALSE(window.startExport("maps/gui_out2.bsp", false, false));   // one at a time
    REQUIRE(waitUntil([&] { return !window.exporting() && window.lastExport(); }, 120000));
    CHECK(errors.list.empty());
    REQUIRE(window.lastExport());
    CHECK(window.lastExport()->ok);
    CHECK(window.lastExport()->bspPath == "maps/gui_out.bsp");
    CHECK(fs::exists(game.root / "maps" / "gui_out.bsp"));
    CHECK(window.statusBar()->currentMessage().startsWith(QStringLiteral("Exported maps/gui_out.bsp")));

    // a path outside the game folder is refused
    CHECK_FALSE(window.startExport("../outside.bsp", false, false));
    REQUIRE(errors.list.size() == 1);

    // Play map without the game set: says where to set it
    window.playMap();
    REQUIRE(errors.list.size() == 2);
    CHECK(errors.list.back().contains(QStringLiteral("Run game")));
    CHECK_FALSE(window.exporting());

    // the start line is kept per game folder
    CHECK(window.launchSettings().exe.empty());
    CHECK(window.launchSettings().args == "--map {mapname}.bsp");
    exporting::LaunchSettings s;
    s.exe = "C:/Games/none/game.exe";
    s.args = "+map {map}";
    s.workingDir = "C:/Games/none";
    window.setLaunchSettings(s);
    CHECK(window.launchSettings().exe == s.exe);
    CHECK(window.launchSettings().args == s.args);
    CHECK(window.launchSettings().workingDir == s.workingDir);

    // Play map with a game that does not exist: the export runs, then the missing program is reported (nothing is started)
    window.playMap();
    REQUIRE(waitUntil([&] { return !window.exporting() && errors.list.size() >= 3; }, 120000));
    CHECK(errors.list.back().contains(QStringLiteral("does not exist")));
    CHECK(fs::exists(game.root / "maps" / "demo_lit.bsp"));
}

TEST_CASE("File > Import Quake map converts a .map file to a new, unsaved map") {
    Game game;
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = game.root.generic_string();
    ui::MainWindow window(sv);   // no job pool: the import runs inline
    Errors errors;
    errors.attach(window);
    const fs::path file = game.root / "cube.map";
    {
        std::ofstream out(file);
        out << "{\n\"classname\" \"worldspawn\"\n{\n"
               "( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 ) dev/grey 0 0 0 0.5 0.5 0 0 0\n"
               "}\n}\n"
               "{\n\"classname\" \"light\"\n\"origin\" \"32 32 96\"\n\"light\" \"300\"\n}\n";
    }
    // a file that is no map is reported and nothing opens
    {
        std::ofstream(game.root / "bad.map") << "this is not a map";
    }
    CHECK(window.importQuakeMap((game.root / "bad.map").generic_string()) == true);
    REQUIRE(errors.list.size() == 1);
    CHECK(window.document() == nullptr);

    REQUIRE(window.importQuakeMap(file.generic_string()));
    REQUIRE(window.document() != nullptr);
    CHECK(errors.list.size() == 1);   // only the failed import of the bad file
    CHECK(window.document()->path.empty());
    CHECK(window.isDirty());
    INFO(window.statusBar()->currentMessage().toStdString());
    CHECK(window.document()->brushCount() == 1);
    CHECK(window.document()->entities.size() == 2);
    CHECK(window.statusBar()->currentMessage().startsWith(QStringLiteral("Imported")));
}
