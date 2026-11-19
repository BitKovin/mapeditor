// T1.6: the Qt adapter (Qt events -> router events) and the shipped default keymap. Headless (offscreen platform), no GPU.
#include <doctest.h>

#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QWheelEvent>

#include <algorithm>
#include <set>
#include <string>

#include "app/actions/keymap.h"
#include "app/actions/registry.h"
#include "app/actions/router.h"
#include "ui/app_actions.h"
#include "ui/input_adapter.h"

using namespace native;
using namespace native::actions;

namespace {

std::string readAsset(const char* relative) {
    QFile f(QString::fromUtf8(NATIVE_ASSETS_DIR) + "/" + QString::fromUtf8(relative));
    REQUIRE_MESSAGE(f.open(QIODevice::ReadOnly), "cannot open ", f.fileName().toStdString());
    return f.readAll().toStdString();
}

// Contexts the application sets (see MainWindow::setContextsFor / viewportContexts): the only names a preset may use.
const std::set<std::string> kKnownContexts = {"global",         "window:main",   "area:viewport",   "view:perspective", "view:fly",
                                              "view:orbit",     "view:ortho",    "hold:camera.fly"};

} // namespace

TEST_CASE("key names: Latin keys, specials, keypad, shifted punctuation and layout characters") {
    CHECK(ui::keyNameFromQt(Qt::Key_A, {}) == "A");
    CHECK(ui::keyNameFromQt(Qt::Key_5, {}) == "5");
    CHECK(ui::keyNameFromQt(Qt::Key_5, Qt::KeypadModifier) == "Numpad 5");
    CHECK(ui::keyNameFromQt(Qt::Key_Plus, Qt::KeypadModifier) == "Numpad +");
    CHECK(ui::keyNameFromQt(Qt::Key_Period, Qt::KeypadModifier) == "Numpad .");
    CHECK(ui::keyNameFromQt(Qt::Key_F5, {}) == "F5");
    CHECK(ui::keyNameFromQt(Qt::Key_F12, {}) == "F12");
    CHECK(ui::keyNameFromQt(Qt::Key_Escape, {}) == "Escape");
    CHECK(ui::keyNameFromQt(Qt::Key_Delete, {}) == "Delete");
    CHECK(ui::keyNameFromQt(Qt::Key_Space, {}) == "Space");
    CHECK(ui::keyNameFromQt(Qt::Key_Backtab, Qt::ShiftModifier) == "Tab");
    CHECK(ui::keyNameFromQt(Qt::Key_Return, {}) == "Enter");
    CHECK(ui::keyNameFromQt(Qt::Key_Return, Qt::KeypadModifier) == "Numpad Enter");
    CHECK(ui::keyNameFromQt(Qt::Key_BracketLeft, {}) == "[");
    CHECK(ui::keyNameFromQt(Qt::Key_Shift, {}) == "Shift");
    CHECK(ui::keyNameFromQt(Qt::Key_Control, {}) == "Ctrl");
    // Shifted punctuation has no binding name: matching falls back to the key's position.
    CHECK(ui::keyNameFromQt(Qt::Key_Exclam, Qt::ShiftModifier).empty());
    CHECK(ui::keyNameFromQt(Qt::Key_Underscore, Qt::ShiftModifier).empty());
    // A Cyrillic letter is kept as UTF-8 (non-ASCII), to be matched through the scan position.
    const std::string ya = ui::keyNameFromQt(0x42F, {});
    CHECK(ya.size() == 2);
    CHECK(static_cast<unsigned char>(ya[0]) >= 0x80);
}

TEST_CASE("scan codes map to the US key positions") {
    CHECK(ui::scanNameFromScanCode(0x10) == "Q");
    CHECK(ui::scanNameFromScanCode(0x11) == "W");
    CHECK(ui::scanNameFromScanCode(0x1E) == "A");
    CHECK(ui::scanNameFromScanCode(0x2C) == "Z");
    CHECK(ui::scanNameFromScanCode(0x02) == "1");
    CHECK(ui::scanNameFromScanCode(0x0B) == "0");
    CHECK(ui::scanNameFromScanCode(0x29) == "`");
    CHECK(ui::scanNameFromScanCode(0x35) == "/");
    CHECK(ui::scanNameFromScanCode(0x01).empty());     // Escape is not a character key
    CHECK(ui::scanNameFromScanCode(0x1C).empty());
    CHECK(ui::scanNameFromScanCode(0x200).empty());
}

TEST_CASE("modifiers exclude the key's own modifier") {
    CHECK(ui::modsFromQt(Qt::ControlModifier | Qt::ShiftModifier) == (kCtrl | kShift));
    CHECK(ui::modsFromQt(Qt::ShiftModifier, Qt::Key_Shift) == 0);
    CHECK(ui::modsFromQt(Qt::ControlModifier | Qt::AltModifier, Qt::Key_Alt) == kCtrl);
    CHECK(ui::modsFromQt(Qt::KeypadModifier) == 0);
}

TEST_CASE("Qt key events become router events") {
    {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        const InputEvent ev = ui::fromKeyEvent(e, true, false);
        CHECK(ev.type == InputEvent::Type::KeyPress);
        CHECK(ev.key == "Z");
        CHECK(ev.scan == "Z");                 // no scan code (synthetic): the Latin key stands for its position
        CHECK(ev.mods == (kCtrl | kShift));
        CHECK_FALSE(ev.autoRepeat);
        CHECK(ev.timeMs > 0);
    }
    {
        // The Cyrillic Й key (US position Q) with Ctrl: the layout character is non-Latin, the position gives Q.
        QKeyEvent e(QEvent::KeyPress, 0x419, Qt::ControlModifier, 0x10, 0, 0, QString(), false, 1);
        const InputEvent ev = ui::fromKeyEvent(e, true, false);
        CHECK(ev.scan == "Q");
        CHECK(static_cast<unsigned char>(ev.key[0]) >= 0x80);
        CHECK(ev.mods == kCtrl);
    }
    {
        QKeyEvent e(QEvent::KeyRelease, Qt::Key_Shift, Qt::ShiftModifier);
        const InputEvent ev = ui::fromKeyEvent(e, false, true);
        CHECK(ev.type == InputEvent::Type::KeyRelease);
        CHECK(ev.key == "Shift");
        CHECK(ev.mods == 0);
        CHECK(ev.textFieldFocused);
    }
    {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier, QStringLiteral("w"), true);
        CHECK(ui::fromKeyEvent(e, true, false).autoRepeat);
    }
}

TEST_CASE("Qt mouse and wheel events become router events") {
    const QPointF local(30, 40);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, Qt::RightButton, Qt::RightButton, Qt::ControlModifier);
    const InputEvent p = ui::fromMouseEvent(press);
    CHECK(p.type == InputEvent::Type::MousePress);
    CHECK(p.button == 2);
    CHECK(p.mods == kCtrl);
    CHECK(p.x == doctest::Approx(30));
    CHECK(p.y == doctest::Approx(40));
    QMouseEvent move(QEvent::MouseMove, local, local, Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    const InputEvent m = ui::fromMouseEvent(move, QPointF(5, -3));
    CHECK(m.type == InputEvent::Type::MouseMove);
    CHECK(m.dx == doctest::Approx(5));
    CHECK(m.dy == doctest::Approx(-3));
    QMouseEvent rel(QEvent::MouseButtonRelease, local, local, Qt::BackButton, Qt::NoButton, Qt::NoModifier);
    CHECK(ui::fromMouseEvent(rel).button == 4);
    CHECK(ui::fromMouseEvent(rel).type == InputEvent::Type::MouseRelease);
    // One notch up (angle +120, away from the user) = -100 in the Shell's units.
    QWheelEvent wheel(local, local, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    const InputEvent w = ui::fromWheelEvent(wheel);
    CHECK(w.type == InputEvent::Type::Wheel);
    CHECK(w.wheel == doctest::Approx(-100));
}

TEST_CASE("text input widgets are recognised") {
    QLineEdit line;
    QTextEdit text;
    QPlainTextEdit plain;
    QSpinBox spin;
    QWidget other;
    CHECK(ui::isTextInput(&line));
    CHECK(ui::isTextInput(&text));
    CHECK(ui::isTextInput(&plain));
    CHECK(ui::isTextInput(&spin));
    CHECK_FALSE(ui::isTextInput(&other));
    CHECK_FALSE(ui::isTextInput(nullptr));
}

TEST_CASE("the default keymap loads, names real actions and contexts, and has no conflicts") {
    Registry registry;
    ui::registerAppActions(registry);
    Keymap keymap;
    std::string error;
    REQUIRE_MESSAGE(keymap.loadPreset(readAsset("keymaps/default.json"), &error), error);
    CHECK(keymap.presetName() == "default");
    CHECK(keymap.effective().size() > 10);
    for (const Binding& b : keymap.effective()) {
        INFO("binding " << b.action << " " << b.context << " " << b.input);
        const ActionDef* def = registry.find(b.action);
        REQUIRE(def != nullptr);
        CHECK(kKnownContexts.count(b.context) == 1);
        // A binding's context is one the action is available in (or the action is available everywhere).
        if (!def->contexts.empty() && b.context != "global")
            CHECK(std::find(def->contexts.begin(), def->contexts.end(), b.context) != def->contexts.end());
    }
    CHECK(keymap.conflicts().empty());
    // The menu text of the keys that exist so far.
    CHECK(keymap.displayText("file.exit") == "Ctrl+Q");
    CHECK(keymap.displayText("edit.undo") == "Ctrl+Z");
    CHECK(keymap.displayText("edit.redo") == "Ctrl+Shift+Z");
    // Every action of the registry has a unique id and a label.
    std::set<std::string> ids;
    for (const ActionDef& d : registry.all()) {
        CHECK_FALSE(d.label.empty());
        CHECK(ids.insert(d.id).second);
    }
}
