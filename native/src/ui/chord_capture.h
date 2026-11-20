// ui/chord_capture -- a line edit that records an input instead of typing it: the next key (with its modifiers), mouse click or drag, or wheel step that reaches it becomes
// the chord text of a binding ("Ctrl+Shift+S", "Shift+LMB drag", "Wheel Up"). A modifier pressed and released alone is recorded as that key ("Shift"). Used by the binding
// recorder and the reverse lookup of Settings > Keymap.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md §5 (record a binding), 08_MILESTONES.md T4.6.
#pragma once

#include <QLineEdit>
#include <QPoint>

#include <string>

#include "app/actions/input.h"

namespace native::ui {

class ChordCapture : public QLineEdit {
    Q_OBJECT
public:
    explicit ChordCapture(QWidget* parent = nullptr);

    // How a recorded key triggers (Press, Hold, Release, DoubleTap); mouse and wheel inputs are always plain.
    void setMode(actions::ChordMode mode) { mode_ = mode; }
    actions::ChordMode mode() const { return mode_; }
    // Record the key by its position on a US keyboard ("phys:W") instead of the character it produces.
    void setPhysical(bool on) { physical_ = on; }
    // While off the field is an ordinary text field (to type a sequence such as "Ctrl+K, Ctrl+S").
    void setCapturing(bool on);
    bool capturing() const { return capturing_; }

    // The chord text an event stands for; empty when it is not something to record (a release, a move, a key without a name).
    static std::string textFor(const actions::InputEvent& event, actions::ChordMode mode, bool physical, bool drag = false);

signals:
    // A chord was recorded (the text is in the field).
    void captured(const QString& chord);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void record(const std::string& text);
    void cancelPending() { pendingModifier_.clear(); }

    actions::ChordMode mode_ = actions::ChordMode::Press;
    bool physical_ = false;
    bool capturing_ = true;
    std::string pendingModifier_;        // a modifier went down and nothing else since
    actions::InputEvent mouseDown_;      // the press of the click in progress
    bool mouseActive_ = false, dragged_ = false;
    QPoint mouseStart_;
};

} // namespace native::ui
