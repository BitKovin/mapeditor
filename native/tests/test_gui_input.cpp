// T1.6 (Qt side): every key and mouse event goes through the action router; the menus are generated from the registry with the keys of
// the keymap; a focused text field gets typed characters while Ctrl chords still reach the router; the user keymap changes keys.
// Real windows: part of native_gui_tests (the cases with viewports also need the GPU).
#include <doctest.h>

#include <DockWidget.h>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTemporaryDir>

#include <nlohmann/json.hpp>

#include "gui_support.h"
#include "render/viewport_window.h"
#include "ui/input_adapter.h"
#include "ui/registry_menus.h"

using namespace native;
using namespace guitest;
using json = nlohmann::json;

namespace {

ui::Services keysOnly() {
    ui::Services sv;
    sv.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
    return sv;
}

QWindow* nativeWindow(QWidget& w) {
    w.winId();
    return w.windowHandle();
}

} // namespace

TEST_CASE("menus are generated from the registry and show the keymap's keys") {
    ui::MainWindow window(keysOnly());
    window.show();
    REQUIRE(window.keymapError().empty());
    ui::RegistryMenus* menus = window.menus();
    REQUIRE(menus != nullptr);
    REQUIRE(menus->menu("&File") != nullptr);
    REQUIRE(menus->menu("&Edit") != nullptr);
    REQUIRE(menus->menu("&View") != nullptr);
    REQUIRE(menus->menu("&View/&Layout") != nullptr);
    REQUIRE(menus->menu("&View/&Camera (active viewport)") != nullptr);

    QAction* exit = menus->entry("file.exit");
    REQUIRE(exit != nullptr);
    CHECK(exit->text() == QStringLiteral("Exit\tCtrl+Q"));
    CHECK(exit->isEnabled());
    // Undo / redo exist with their keys but are disabled until a document exists.
    QAction* undo = menus->entry("edit.undo");
    REQUIRE(undo != nullptr);
    CHECK(undo->text() == QStringLiteral("Undo\tCtrl+Z"));
    CHECK_FALSE(undo->isEnabled());
    CHECK_FALSE(menus->entry("edit.redo")->isEnabled());
    // No viewports without render services: the layout entries are disabled, the stats toggle works and is checked.
    CHECK_FALSE(menus->entry("view.layout", json{{"count", 2}})->isEnabled());
    QAction* stats = menus->entry("view.stats.toggle");
    REQUIRE(stats != nullptr);
    CHECK(stats->isCheckable());
    CHECK(stats->isChecked());
    stats->trigger();
    CHECK(window.statsDock()->isClosed());
    CHECK_FALSE(stats->isChecked());
    stats->trigger();
    CHECK_FALSE(window.statsDock()->isClosed());
    CHECK(stats->isChecked());
    // The menu bar shows the generated menus in registration order.
    QStringList titles;
    for (QAction* a : window.menuBar()->actions()) titles << a->text();
    CHECK(titles == QStringList({QStringLiteral("&File"), QStringLiteral("&Edit"), QStringLiteral("&View"), QStringLiteral("&Tools")}));
}

TEST_CASE("layout and camera menu entries act on the window and the active viewport" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(1000, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        ui::RegistryMenus* menus = window.menus();
        QAction* two = menus->entry("view.layout", json{{"count", 2}});
        QAction* one = menus->entry("view.layout", json{{"count", 1}});
        REQUIRE(two != nullptr);
        CHECK(two->isEnabled());
        CHECK(one->isChecked());
        CHECK_FALSE(two->isChecked());
        two->trigger();
        REQUIRE(waitUntil([&] { return window.viewports().size() == 2 && allReady(window); }));
        CHECK(two->isChecked());
        CHECK_FALSE(one->isChecked());

        // The camera entries act on the active viewport (the last one a mouse button was pressed in).
        render::ViewportWindow* top = window.viewports()[1].window;
        sendMouse(top, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, QPoint(10, 10));
        sendMouse(top, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, QPoint(10, 10));
        CHECK(window.activeViewport() == top);
        QAction* orbit = menus->entry("camera.mode", json{{"mode", "orbit"}});
        REQUIRE(orbit != nullptr);
        menus->refresh();
        CHECK(menus->entry("camera.mode", json{{"mode", "top"}})->isChecked());
        CHECK_FALSE(orbit->isChecked());
        orbit->trigger();
        CHECK(top->rig().mode() == render::CameraMode::Orbit);
        CHECK(orbit->isChecked());
        CHECK(window.viewports()[0].window->rig().mode() == render::CameraMode::Fly);
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}

TEST_CASE("keys reach the router: Ctrl+Q exits, the user keymap rebinds it") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString userFile = dir.filePath("keymaps/default.json");
    QDir().mkpath(QFileInfo(userFile).absolutePath());
    QFile f(userFile);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(R"({"base":"default","version":1,
                "bind":[{"action":"file.exit","context":"global","input":"Ctrl+W"}],
                "unbind":[{"action":"file.exit","context":"global","input":"Ctrl+Q"}]})");
    f.close();

    {
        ui::MainWindow window(keysOnly());
        window.show();
        REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
        sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier);
        CHECK_FALSE(window.isVisible());
    }
    {
        ui::Services sv = keysOnly();
        sv.userKeymapPath = userFile.toStdString();
        ui::MainWindow window(sv);
        window.show();
        REQUIRE(window.keymapError().empty());
        REQUIRE(waitUntil([&] { return window.windowHandle() != nullptr && window.isVisible(); }));
        CHECK(window.menus()->entry("file.exit")->text() == QStringLiteral("Exit\tCtrl+W"));
        sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier);   // unbound now
        CHECK(window.isVisible());
        // Shift+Ctrl+W is not Ctrl+W: exact modifiers.
        sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(window.isVisible());
        sendKey(window.windowHandle(), QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
        CHECK_FALSE(window.isVisible());
    }
    {
        // A broken user keymap is reported and ignored: the preset keys still work.
        QFile bad(userFile);
        REQUIRE(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bad.write(R"({"bind":[{"action":"file.exit","input":"Ctrl+Banana"}]})");
        bad.close();
        ui::Services sv = keysOnly();
        sv.userKeymapPath = userFile.toStdString();
        ui::MainWindow window(sv);
        CHECK_FALSE(window.keymapError().empty());
        CHECK(window.keymap().displayText("file.exit") == "Ctrl+Q");
    }
}

TEST_CASE("the user keymap is saved and read back") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    ui::Services sv = keysOnly();
    sv.userKeymapPath = dir.filePath("a/b/default.json").toStdString();   // folders are created on demand
    {
        ui::MainWindow window(sv);
        actions::Binding b;
        REQUIRE(actions::makeBinding("file.exit", "global", "Ctrl+Alt+X", nullptr, false, b));
        window.keymap().bindUser(b);
        REQUIRE(window.saveUserKeymap());
    }
    {
        ui::MainWindow window(sv);
        CHECK(window.keymapError().empty());
        CHECK(window.keymap().bindingsFor("file.exit").size() == 2);   // Ctrl+Q from the preset, Ctrl+Alt+X from the user layer
    }
}

TEST_CASE("a focused text field gets typed characters; Ctrl chords still reach the router") {
    ui::MainWindow window(keysOnly());
    int ran = 0;
    actions::ActionDef def;
    def.id = "test.letter";
    def.label = "Test letter";
    def.run = [&ran](actions::ActionEnv&, const actions::Invocation&) { ++ran; };
    REQUIRE(window.registry().add(def));
    actions::Binding plain, ctrl;
    REQUIRE(actions::makeBinding("test.letter", "global", "T", nullptr, false, plain));
    REQUIRE(actions::makeBinding("test.letter", "global", "Ctrl+T", nullptr, false, ctrl));
    window.keymap().bindUser(plain);
    window.keymap().bindUser(ctrl);
    window.show();

    QLineEdit edit;
    edit.show();
    edit.activateWindow();
    edit.setFocus();
    REQUIRE(waitUntil([&] { return QApplication::focusWidget() == &edit; }));
    QWindow* w = nativeWindow(edit);
    REQUIRE(w != nullptr);

    QKeyEvent typeT(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier, QStringLiteral("t"));
    QApplication::sendEvent(w, &typeT);
    CHECK(ran == 0);                         // typed into the field
    CHECK(edit.text() == QStringLiteral("t"));
    QKeyEvent ctrlT(QEvent::KeyPress, Qt::Key_T, Qt::ControlModifier);
    QApplication::sendEvent(w, &ctrlT);
    CHECK(ran == 1);                         // the chord goes to the router

    // With the focus elsewhere the plain key is an action.
    edit.clearFocus();
    window.activateWindow();
    window.setFocus();
    REQUIRE(waitUntil([&] { return !ui::isTextInput(QApplication::focusWidget()); }));
    QKeyEvent plainT(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier, QStringLiteral("t"));
    QApplication::sendEvent(window.windowHandle(), &plainT);
    CHECK(ran == 2);
    CHECK(edit.text() == QStringLiteral("t"));
}

TEST_CASE("Shift boosts the flight; releasing the look button ends the flight" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(900, 600);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        pumpFor(200);
        render::ViewportWindow* persp = window.viewport();
        const QPoint mid(persp->width() / 2, persp->height() / 2);
        persp->rig().fly().position = {0, 2, 20};
        persp->rig().fly().yaw = 3.14159265358979;
        persp->rig().fly().pitch = 0;
        persp->viewChanged();

        sendMouse(persp, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, mid);
        REQUIRE(window.router().holding());
        // Shift before the key: boost on; the key then flies. Shift released: boost off. Timing of the flight itself is loose on a busy
        // machine, so the boost is checked as state and the distance only against the speed limit.
        const render::Vec3 p0 = persp->rig().fly().position;
        QElapsedTimer t;
        t.start();
        CHECK_FALSE(persp->rig().boosting());
        sendKey(persp, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        CHECK(persp->rig().boosting());
        sendKey(persp, QEvent::KeyPress, Qt::Key_W, Qt::ShiftModifier);
        CHECK(persp->rig().boosting());
        pumpFor(400);
        sendKey(persp, QEvent::KeyRelease, Qt::Key_W, Qt::ShiftModifier);
        sendKey(persp, QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        CHECK_FALSE(persp->rig().boosting());
        const double dt = t.elapsed() / 1000.0;
        const double flown = render::length(persp->rig().fly().position - p0);
        INFO("flown " << flown << " m in " << dt << " s");
        CHECK(flown > 0);
        CHECK(flown < 3.0 * 8.0 * dt * 1.05);

        // Shift pressed after the key: boost on mid-flight (the key was pressed with Shift up).
        sendKey(persp, QEvent::KeyPress, Qt::Key_W);
        CHECK_FALSE(persp->rig().boosting());
        sendKey(persp, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        CHECK(persp->rig().boosting());
        sendKey(persp, QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        CHECK_FALSE(persp->rig().boosting());
        sendKey(persp, QEvent::KeyRelease, Qt::Key_W);

        sendKey(persp, QEvent::KeyPress, Qt::Key_W);
        pumpFor(40);
        // Releasing the look button ends the flight even if the key is still down.
        sendMouse(persp, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, mid);
        CHECK_FALSE(window.router().holding("camera.look"));
        const render::Vec3 p1 = persp->rig().fly().position;
        pumpFor(120);
        CHECK(render::length(persp->rig().fly().position - p1) == 0.0);
        sendKey(persp, QEvent::KeyRelease, Qt::Key_W);
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}
