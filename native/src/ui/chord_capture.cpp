// ui/chord_capture -- see chord_capture.h.
#include "ui/chord_capture.h"

#include <QApplication>
#include <QEvent>

#include "ui/input_adapter.h"

namespace native::ui {

namespace {
bool isModifierName(const std::string& k) { return k == "Shift" || k == "Ctrl" || k == "Alt" || k == "Meta"; }
constexpr int kDragPixels = 4;
} // namespace

ChordCapture::ChordCapture(QWidget* parent) : QLineEdit(parent) {
    setReadOnly(true);
    setFocusPolicy(Qt::StrongFocus);
    setPlaceholderText(QStringLiteral("Click here, then press the input"));
    setCursor(Qt::PointingHandCursor);
}

void ChordCapture::setCapturing(bool on) {
    capturing_ = on;
    setReadOnly(on);
    setCursor(on ? Qt::PointingHandCursor : Qt::IBeamCursor);
    cancelPending();
}

std::string ChordCapture::textFor(const actions::InputEvent& e, actions::ChordMode mode, bool physical, bool drag) {
    using actions::InputEvent;
    actions::Chord chord;
    actions::InputStep step;
    step.mods = e.mods;
    switch (e.type) {
        case InputEvent::Type::KeyPress: {
            std::string key = e.key;
            if (physical && !e.scan.empty()) {
                key = e.scan;
                step.physical = true;
            } else if (key.empty()) {
                key = e.scan;   // shifted punctuation has no name of its own: its position stands in
            }
            if (key.empty()) return {};
            step.type = actions::InputStep::Type::Key;
            step.key = key;
            chord.mode = mode;
            break;
        }
        case InputEvent::Type::MouseRelease:
        case InputEvent::Type::MousePress:
            if (e.button < 1) return {};
            step.type = actions::InputStep::Type::Mouse;
            step.button = e.button;
            step.drag = drag;
            break;
        case InputEvent::Type::Wheel:
            if (e.wheel == 0) return {};
            step.type = actions::InputStep::Type::Wheel;
            step.wheelDir = e.wheel > 0 ? -1 : 1;   // the event counts towards the user (down) as positive
            break;
        default:
            return {};
    }
    chord.steps.push_back(step);
    return actions::formatChord(chord);
}

void ChordCapture::record(const std::string& text) {
    if (text.empty()) return;
    setText(QString::fromStdString(text));
    emit captured(QString::fromStdString(text));
}

bool ChordCapture::event(QEvent* e) {
    if (capturing_ && e->type() == QEvent::ShortcutOverride) {   // keys that would be shortcuts (or focus changes) belong to the recording
        e->accept();
        return true;
    }
    if (capturing_ && e->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {   // the focus keys are recorded too (Tab is a binding like any other)
            keyPressEvent(const_cast<QKeyEvent*>(k));
            return true;
        }
    }
    return QLineEdit::event(e);
}

void ChordCapture::keyPressEvent(QKeyEvent* e) {
    if (!capturing_) {
        QLineEdit::keyPressEvent(e);
        return;
    }
    if (e->isAutoRepeat()) return;
    const actions::InputEvent ev = fromKeyEvent(*e, true, false);
    if (isModifierName(ev.key)) {   // wait: a modifier alone is a binding only when nothing else follows
        pendingModifier_ = ev.key;
        return;
    }
    cancelPending();
    record(textFor(ev, mode_, physical_));
}

void ChordCapture::keyReleaseEvent(QKeyEvent* e) {
    if (!capturing_) {
        QLineEdit::keyReleaseEvent(e);
        return;
    }
    if (e->isAutoRepeat()) return;
    const actions::InputEvent ev = fromKeyEvent(*e, false, false);
    if (!pendingModifier_.empty() && ev.key == pendingModifier_) {
        actions::InputEvent down = ev;
        down.type = actions::InputEvent::Type::KeyPress;
        down.mods = 0;
        cancelPending();
        record(textFor(down, mode_, physical_));
    }
}

void ChordCapture::mousePressEvent(QMouseEvent* e) {
    if (!capturing_) {
        QLineEdit::mousePressEvent(e);
        return;
    }
    if (!hasFocus()) {   // the first click only gives the field the keyboard
        setFocus(Qt::MouseFocusReason);
        mouseActive_ = false;
        return;
    }
    cancelPending();
    mouseDown_ = fromMouseEvent(*e);
    mouseDown_.type = actions::InputEvent::Type::MousePress;
    mouseStart_ = e->position().toPoint();
    mouseActive_ = true;
    dragged_ = false;
}

void ChordCapture::mouseMoveEvent(QMouseEvent* e) {
    if (!capturing_) {
        QLineEdit::mouseMoveEvent(e);
        return;
    }
    if (mouseActive_ && (e->position().toPoint() - mouseStart_).manhattanLength() > kDragPixels) dragged_ = true;   // a drag is recorded by moving with the button held
}

void ChordCapture::mouseReleaseEvent(QMouseEvent* e) {
    if (!capturing_) {
        QLineEdit::mouseReleaseEvent(e);
        return;
    }
    if (!mouseActive_) return;
    mouseActive_ = false;
    record(textFor(mouseDown_, actions::ChordMode::Press, false, dragged_));
}

void ChordCapture::wheelEvent(QWheelEvent* e) {
    if (!capturing_) {
        QLineEdit::wheelEvent(e);
        return;
    }
    cancelPending();
    record(textFor(fromWheelEvent(*e), actions::ChordMode::Press, false));
}

} // namespace native::ui
