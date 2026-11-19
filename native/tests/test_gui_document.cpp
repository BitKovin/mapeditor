// T2.2 (Qt side): File > Open / Save / Save As / Open recent / assets folder through the main window, on the job pool and inline.
// Real windows (native_gui_tests); no GPU needed.
#include <doctest.h>

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "base/jobs.h"
#include "doc/document.h"
#include "doc/io.h"
#include "gui_support.h"
#include "ui/registry_menus.h"

namespace fs = std::filesystem;
using namespace native;
using namespace guitest;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A scratch assets folder with copies of two shipped maps (so saving never touches the repository).
struct Assets {
    QTemporaryDir dir;
    fs::path root;
    Assets() {
        root = fs::path(dir.path().toStdString());
        fs::create_directories(root / "maps");
        for (const char* name : {"demo.map.json", "engine_test.map.json"})
            fs::copy_file(fs::path(NATIVE_CORE_ASSETS_DIR) / "maps" / name, root / "maps" / name);
    }
};

ui::Services docServices(const Assets& a, base::JobPool* jobs) {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    sv.assetsRoot = a.root.generic_string();
    sv.jobs = jobs;
    return sv;
}

// MainWindow::windowTitle() (static, the base title) hides QWidget::windowTitle().
QString titleOf(const ui::MainWindow& w) { return static_cast<const QWidget&>(w).windowTitle(); }

struct Errors {
    std::vector<QString> list;
    void attach(ui::MainWindow& w) { w.setErrorHandler([this](const QString& m) { list.push_back(m); }); }
};

} // namespace

TEST_CASE("opening a map loads it on the job pool, updates the title and keeps the recent list") {
    Assets assets;
    base::JobPool jobs(2);
    ui::Services sv = docServices(assets, &jobs);
    ui::MainWindow window(sv);
    Errors errors;
    errors.attach(window);
    window.show();
    int changes = 0;
    QObject::connect(&window, &ui::MainWindow::documentChanged, [&] { ++changes; });
    CHECK(window.document() == nullptr);
    CHECK(titleOf(window) == QString::fromLatin1(ui::MainWindow::windowTitle()));

    window.openDocument("maps/demo.map.json");
    CHECK(window.busy());
    REQUIRE(waitUntil([&] { return window.document() != nullptr; }, 10000));
    CHECK_FALSE(window.busy());
    CHECK(errors.list.empty());
    CHECK(changes == 1);
    CHECK(window.document()->path == "maps/demo.map.json");
    CHECK(window.document()->brushCount() > 0);
    CHECK_FALSE(window.isDirty());
    CHECK(titleOf(window).startsWith(QStringLiteral("maps/demo.map.json - ")));
    REQUIRE(window.recentFiles().size() == 1);
    CHECK(window.recentFiles()[0].find("demo.map.json") != std::string::npos);
    CHECK(window.statusBar()->currentMessage().startsWith(QStringLiteral("Loaded maps/demo.map.json")));

    // A bad open (outside the assets folder, missing, not JSON) reports and leaves the document alone.
    const auto before = window.document();
    window.openDocument("../outside.map.json");
    CHECK(errors.list.size() == 1);
    CHECK(errors.list.back().contains(QStringLiteral("not inside the assets folder")));
    window.openDocument("maps/missing.map.json");
    REQUIRE(waitUntil([&] { return errors.list.size() == 2 && !window.busy(); }, 10000));
    CHECK(errors.list.back().contains(QStringLiteral("cannot open")));
    {
        std::ofstream(assets.root / "maps" / "broken.map.json") << "{ nope";
    }
    window.openDocument("maps/broken.map.json");
    REQUIRE(waitUntil([&] { return errors.list.size() == 3 && !window.busy(); }, 10000));
    CHECK(window.document() == before);
    CHECK(window.recentFiles().size() == 1);   // failed opens are not remembered

    // A second map; the recent list is newest first and has no duplicates.
    window.openDocument("maps/engine_test.map.json");
    REQUIRE(waitUntil([&] { return window.document() != before && !window.busy(); }, 10000));
    window.openDocument("maps/demo.map.json");
    REQUIRE(waitUntil([&] { return window.document()->path == "maps/demo.map.json" && !window.busy(); }, 10000));
    REQUIRE(window.recentFiles().size() == 2);
    CHECK(window.recentFiles()[0].find("demo.map.json") != std::string::npos);
    CHECK(window.recentFiles()[1].find("engine_test.map.json") != std::string::npos);
}

TEST_CASE("without a job pool loads and saves run inline; an absolute path inside the root works") {
    Assets assets;
    ui::MainWindow window(docServices(assets, nullptr));
    Errors errors;
    errors.attach(window);
    window.openDocument((assets.root / "maps" / "demo.map.json").generic_string());
    REQUIRE(window.document() != nullptr);
    CHECK(window.document()->path == "maps/demo.map.json");
    CHECK(errors.list.empty());
    CHECK_FALSE(window.busy());
}

TEST_CASE("opening needs an assets folder") {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    ui::MainWindow window(sv);
    Errors errors;
    errors.attach(window);
    window.openDocument("maps/demo.map.json");
    REQUIRE(errors.list.size() == 1);
    CHECK(errors.list[0].contains(QStringLiteral("assets folder")));
    CHECK(window.document() == nullptr);
}

TEST_CASE("Save writes the file; Save As moves the document to a new path") {
    Assets assets;
    base::JobPool jobs(2);
    ui::MainWindow window(docServices(assets, &jobs));
    Errors errors;
    errors.attach(window);
    window.openDocument("maps/demo.map.json");
    REQUIRE(waitUntil([&] { return window.document() != nullptr; }, 10000));
    const fs::path file = assets.root / "maps" / "demo.map.json";
    const std::string canonical = doc::serializeDocument(*window.document());

    // Save rewrites the file in the editor's format.
    CHECK(slurp(file) != canonical);           // the shipped file has a generator's formatting
    window.saveDocument();
    REQUIRE(waitUntil([&] { return !window.busy(); }, 10000));
    CHECK(slurp(file) == canonical);
    CHECK_FALSE(fs::exists(fs::path(file.string() + ".tmp")));
    CHECK_FALSE(window.isDirty());

    // Save As: a new file, the document takes its path and the title follows.
    const auto oldVersion = window.document();
    window.saveDocumentAs("maps/sub/copy.map.json");
    REQUIRE(waitUntil([&] { return !window.busy(); }, 10000));
    CHECK(errors.list.empty());
    CHECK(slurp(assets.root / "maps" / "sub" / "copy.map.json") == canonical);
    CHECK(window.document()->path == "maps/sub/copy.map.json");
    CHECK(window.document()->version > oldVersion->version);
    CHECK(window.document()->entities[0].get() == oldVersion->entities[0].get());   // nodes are shared, not copied
    CHECK_FALSE(window.isDirty());
    CHECK(titleOf(window).startsWith(QStringLiteral("maps/sub/copy.map.json - ")));
    CHECK(window.recentFiles()[0].find("copy.map.json") != std::string::npos);

    // Saving outside the assets folder is refused.
    window.saveDocumentAs("../escape.map.json");
    CHECK(errors.list.size() == 1);
    CHECK(window.document()->path == "maps/sub/copy.map.json");
    // A save that cannot write (the target is a folder) reports the problem and keeps the document and its path.
    fs::create_directories(assets.root / "maps" / "isdir.map.json");
    window.saveDocumentAs("maps/isdir.map.json");
    REQUIRE(waitUntil([&] { return !window.busy() && errors.list.size() == 2; }, 10000));
    CHECK(window.document()->path == "maps/sub/copy.map.json");
}

TEST_CASE("the File menu: Open / Save / Save As states, Ctrl+S, the recent submenu") {
    Assets assets;
    base::JobPool jobs(2);
    ui::MainWindow window(docServices(assets, &jobs));
    Errors errors;
    errors.attach(window);
    window.show();
    ui::RegistryMenus* menus = window.menus();
    REQUIRE(menus != nullptr);
    QAction* open = menus->entry("file.open");
    QAction* save = menus->entry("file.save");
    QAction* saveAs = menus->entry("file.save_as");
    REQUIRE((open && save && saveAs));
    CHECK(open->text() == QStringLiteral("Open...\tCtrl+O"));
    CHECK(save->text() == QStringLiteral("Save\tCtrl+S"));
    CHECK(saveAs->text() == QStringLiteral("Save As...\tCtrl+Shift+S"));
    menus->refresh();
    CHECK(open->isEnabled());
    CHECK_FALSE(save->isEnabled());            // no document yet
    CHECK_FALSE(saveAs->isEnabled());

    QMenu* recent = menus->menu("&File/Open &Recent");
    REQUIRE(recent != nullptr);
    emit recent->aboutToShow();
    REQUIRE(recent->actions().size() == 1);
    CHECK(recent->actions()[0]->text() == QStringLiteral("(none)"));
    CHECK_FALSE(recent->actions()[0]->isEnabled());

    window.openDocument("maps/demo.map.json");
    REQUIRE(waitUntil([&] { return window.document() != nullptr && !window.busy(); }, 10000));
    CHECK(save->isEnabled());
    CHECK(saveAs->isEnabled());
    window.openDocument("maps/engine_test.map.json");
    REQUIRE(waitUntil([&] { return window.document()->path == "maps/engine_test.map.json" && !window.busy(); }, 10000));

    // The recent submenu lists both, newest first; choosing the older one opens it.
    emit recent->aboutToShow();
    REQUIRE(recent->actions().size() == 2);
    CHECK(recent->actions()[0]->text().contains(QStringLiteral("engine_test.map.json")));
    CHECK(recent->actions()[1]->text().contains(QStringLiteral("demo.map.json")));
    recent->actions()[1]->trigger();
    REQUIRE(waitUntil([&] { return window.document()->path == "maps/demo.map.json" && !window.busy(); }, 10000));

    // Ctrl+S saves: damage the file, press the key, the editor's format is back.
    const fs::path file = assets.root / "maps" / "demo.map.json";
    const std::string canonical = doc::serializeDocument(*window.document());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "damaged";
    REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
    sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier);
    const bool saved = waitUntil([&] { return slurp(file) == canonical; }, 10000);
    INFO("errors: " << (errors.list.empty() ? std::string("none") : errors.list.back().toStdString()));
    REQUIRE(saved);
    CHECK(errors.list.empty());
}

TEST_CASE("the assets folder and the recent files are remembered") {
    Assets assets;
    QTemporaryDir cfg;
    const QString ini = cfg.filePath("settings.ini");
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::Services sv = docServices(assets, nullptr);
        sv.assetsRoot.clear();              // not given on the command line: the saved setting is used (none yet)
        sv.settings = &settings;
        ui::MainWindow window(sv);
        CHECK(window.assetsRoot().empty());
        window.setAssetsRoot(assets.root.string());
        window.openDocument("maps/demo.map.json");
        REQUIRE(window.document() != nullptr);
        window.close();
    }
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::Services sv = docServices(assets, nullptr);
        sv.assetsRoot.clear();
        sv.settings = &settings;
        ui::MainWindow window(sv);
        CHECK_FALSE(window.assetsRoot().empty());
        REQUIRE(window.recentFiles().size() == 1);
        CHECK(window.recentFiles()[0].find("demo.map.json") != std::string::npos);
        window.openDocument("maps/engine_test.map.json");   // the saved root is in use
        CHECK(window.document() != nullptr);
    }
}
