// ui/input_adapter -- turns Qt input events into the router's InputEvent.
//
// Owns: key naming (the character the active layout produced, the key's position on a US keyboard from the native scan code, so
// non-Latin layouts still match Latin bindings), modifier bits without the key's own modifier, mouse button numbers, the wheel in
// Shell units (~100 per notch, positive = towards the user) and the "a text field has the keyboard" test.
// Thread: UI thread. Pure functions; no state.
// Spec: 10_UI_AND_INPUT.md §4, 08_MILESTONES.md T1.6.
#pragma once

#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>
#include <QWheelEvent>
#include <QWidget>

#include <cstdint>
#include <string>

#include "app/actions/input.h"

namespace native::ui {

// Canonical key name for a Qt key code ("A", "5", "F5", "Escape", "Numpad 5", "Numpad +", a layout character as UTF-8); empty for keys
// no binding can name (shifted punctuation such as '!', dead keys).
std::string keyNameFromQt(int qtKey, Qt::KeyboardModifiers mods);
// Name of the key at a Windows scan code on a US layout ("Z" for scan 0x2C whatever the layout produces); empty when unknown.
// `extended` is the E0 prefix (arrows, numpad Enter...).
std::string scanNameFromScanCode(uint32_t scanCode);
uint8_t modsFromQt(Qt::KeyboardModifiers mods, int ownKey = 0);
int buttonFromQt(Qt::MouseButton b);
// Milliseconds on a monotonic clock (what InputEvent::timeMs holds).
int64_t nowMs();
// A widget that wants typed characters (line edit, text edit, spin box...).
bool isTextInput(const QWidget* w);

actions::InputEvent fromKeyEvent(const QKeyEvent& e, bool press, bool textFieldFocused);
// `delta` = pointer motion since the last event (for the locked pointer: since the lock position).
actions::InputEvent fromMouseEvent(const QMouseEvent& e, QPointF delta = {});
actions::InputEvent fromWheelEvent(const QWheelEvent& e);

} // namespace native::ui
