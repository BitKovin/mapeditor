// ui/input_adapter -- see input_adapter.h.
#include "ui/input_adapter.h"

#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <chrono>

namespace native::ui {

namespace {

// Windows scan codes (set 1) of the character keys, by their US-layout names. Index = scan code; empty = not a character key.
const char* const kScanNames[0x36] = {
    "",  "",  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "",  "",    // 0x00 - 0x0F
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "",  "",  "A", "S",   // 0x10 - 0x1F
    "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "",  "\\", "Z", "X", "C", "V",  // 0x20 - 0x2F
    "B", "N", "M", ",", ".", "/"};                                                     // 0x30 - 0x35

}  // namespace

std::string keyNameFromQt(int key, Qt::KeyboardModifiers mods) {
    const bool keypad = mods.testFlag(Qt::KeypadModifier);
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return std::string(1, static_cast<char>(key));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return (keypad ? "Numpad " : "") + std::string(1, static_cast<char>(key));
    if (keypad) {
        switch (key) {
            case Qt::Key_Plus: return "Numpad +";
            case Qt::Key_Minus: return "Numpad -";
            case Qt::Key_Asterisk: return "Numpad *";
            case Qt::Key_Slash: return "Numpad /";
            case Qt::Key_Period: case Qt::Key_Comma: return "Numpad .";
            case Qt::Key_Enter: case Qt::Key_Return: return "Numpad Enter";
            default: break;
        }
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return "F" + std::to_string(key - Qt::Key_F1 + 1);
    switch (key) {
        case Qt::Key_QuoteLeft: return "`";
        case Qt::Key_Minus: return "-";
        case Qt::Key_Equal: return "=";
        case Qt::Key_BracketLeft: return "[";
        case Qt::Key_BracketRight: return "]";
        case Qt::Key_Backslash: return "\\";
        case Qt::Key_Semicolon: return ";";
        case Qt::Key_Apostrophe: return "'";
        case Qt::Key_Comma: return ",";
        case Qt::Key_Period: return ".";
        case Qt::Key_Slash: return "/";
        case Qt::Key_Space: return "Space";
        case Qt::Key_Tab: case Qt::Key_Backtab: return "Tab";
        case Qt::Key_Return: case Qt::Key_Enter: return "Enter";
        case Qt::Key_Escape: return "Escape";
        case Qt::Key_Backspace: return "Backspace";
        case Qt::Key_Delete: return "Delete";
        case Qt::Key_Insert: return "Insert";
        case Qt::Key_Home: return "Home";
        case Qt::Key_End: return "End";
        case Qt::Key_PageUp: return "PageUp";
        case Qt::Key_PageDown: return "PageDown";
        case Qt::Key_Left: return "Left";
        case Qt::Key_Right: return "Right";
        case Qt::Key_Up: return "Up";
        case Qt::Key_Down: return "Down";
        case Qt::Key_Shift: return "Shift";
        case Qt::Key_Control: return "Ctrl";
        case Qt::Key_Alt: return "Alt";
        case Qt::Key_Meta: return "Meta";
        case Qt::Key_CapsLock: return "CapsLock";
        default: break;
    }
    // A character of the active layout that is not ASCII (Cyrillic, accented...): kept as UTF-8; matching falls back to its US position.
    if (key >= 0x80 && key < 0x01000000) {
        const char32_t ch = static_cast<char32_t>(key);
        return QString::fromUcs4(&ch, 1).toStdString();
    }
    return {};
}

std::string scanNameFromScanCode(uint32_t scanCode) {
    return scanCode < sizeof(kScanNames) / sizeof(kScanNames[0]) ? kScanNames[scanCode] : "";
}

uint8_t modsFromQt(Qt::KeyboardModifiers mods, int ownKey) {
    uint8_t m = 0;
    if (mods.testFlag(Qt::ControlModifier) && ownKey != Qt::Key_Control) m |= actions::kCtrl;
    if (mods.testFlag(Qt::ShiftModifier) && ownKey != Qt::Key_Shift) m |= actions::kShift;
    if (mods.testFlag(Qt::AltModifier) && ownKey != Qt::Key_Alt) m |= actions::kAlt;
    if (mods.testFlag(Qt::MetaModifier) && ownKey != Qt::Key_Meta) m |= actions::kMeta;
    return m;
}

int buttonFromQt(Qt::MouseButton b) {
    switch (b) {
        case Qt::LeftButton: return 1;
        case Qt::RightButton: return 2;
        case Qt::MiddleButton: return 3;
        case Qt::BackButton: return 4;
        case Qt::ForwardButton: return 5;
        default: return 0;
    }
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isTextInput(const QWidget* w) {
    return w && (qobject_cast<const QLineEdit*>(w) || qobject_cast<const QTextEdit*>(w) || qobject_cast<const QPlainTextEdit*>(w) ||
                 qobject_cast<const QAbstractSpinBox*>(w));
}

actions::InputEvent fromKeyEvent(const QKeyEvent& e, bool press, bool textFieldFocused) {
    actions::InputEvent ev;
    ev.type = press ? actions::InputEvent::Type::KeyPress : actions::InputEvent::Type::KeyRelease;
    ev.mods = modsFromQt(e.modifiers(), e.key());
    ev.key = keyNameFromQt(e.key(), e.modifiers());
    // The key's US-layout position: from the scan code (low byte; the extended bit is not a character key), else the Latin key itself.
    ev.scan = scanNameFromScanCode(e.nativeScanCode() & 0xFF);
    if (e.nativeScanCode() > 0xFF) ev.scan.clear();
    if (ev.scan.empty() && !ev.key.empty() && static_cast<unsigned char>(ev.key[0]) < 0x80) ev.scan = ev.key;
    ev.autoRepeat = e.isAutoRepeat();
    ev.textFieldFocused = textFieldFocused;
    ev.timeMs = nowMs();
    return ev;
}

actions::InputEvent fromMouseEvent(const QMouseEvent& e, QPointF delta) {
    actions::InputEvent ev;
    switch (e.type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: ev.type = actions::InputEvent::Type::MousePress; break;
        case QEvent::MouseButtonRelease: ev.type = actions::InputEvent::Type::MouseRelease; break;
        default: ev.type = actions::InputEvent::Type::MouseMove; break;
    }
    ev.button = buttonFromQt(e.button());
    ev.doubleClick = e.type() == QEvent::MouseButtonDblClick;
    ev.mods = modsFromQt(e.modifiers());
    ev.x = e.position().x();
    ev.y = e.position().y();
    ev.dx = delta.x();
    ev.dy = delta.y();
    ev.timeMs = nowMs();
    if (ev.type == actions::InputEvent::Type::MouseMove) {   // what the system says is down: the router ends holds whose button is not
        const Qt::MouseButtons b = e.buttons();
        for (const Qt::MouseButton q : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton})
            if (b.testFlag(q)) ev.buttons = static_cast<uint8_t>(ev.buttons | (1u << (buttonFromQt(q) - 1)));
        ev.buttonsKnown = true;
    }
    return ev;
}

actions::InputEvent fromWheelEvent(const QWheelEvent& e) {
    actions::InputEvent ev;
    ev.type = actions::InputEvent::Type::Wheel;
    ev.mods = modsFromQt(e.modifiers());
    ev.x = e.position().x();
    ev.y = e.position().y();
    // Qt: 120 per notch, positive = away from the user. Shell (browser): ~100 per notch, positive = towards the user.
    ev.wheel = -e.angleDelta().y() * (100.0 / 120.0);
    ev.timeMs = nowMs();
    return ev;
}

} // namespace native::ui
