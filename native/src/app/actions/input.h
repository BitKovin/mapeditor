// app/actions/input -- input chords (what a binding listens for) and input events (what the router is fed).
//
// Owns: the chord grammar of 10_UI_AND_INPUT.md §4.1 (key with modifiers, 2-step sequence, Hold / Release / Double, mouse button
// click or drag, wheel; logical or `phys:` keys), parsing and printing it, and matching an event against one step. Qt-free: the
// Qt adapter (ui/input_adapter) turns QKeyEvent / QMouseEvent / QWheelEvent into InputEvent.
// Thread: any (pure functions).
// Spec: 10_UI_AND_INPUT.md §4, 08_MILESTONES.md T1.6.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace native::actions {

enum Modifier : uint8_t { kCtrl = 1, kShift = 2, kAlt = 4, kMeta = 8 };

// One input of a chord: a key (logical = the character the layout produces, or physical = its position on a US keyboard), a mouse
// button (click, or drag) or the wheel.
struct InputStep {
    enum class Type { Key, Mouse, Wheel };
    Type type = Type::Key;
    uint8_t mods = 0;
    std::string key;        // Key: canonical name ("A", "5", "F5", "Delete", "Space", "Numpad .", "Shift" ...)
    bool physical = false;  // Key: matched by position (`phys:W`)
    int button = 0;         // Mouse: 1 LMB, 2 RMB, 3 MMB, 4 / 5 side buttons
    bool drag = false;      // Mouse: bound to press-and-move (a Hold)
    int wheelDir = 0;       // Wheel: +1 up, -1 down, 0 either
    bool operator==(const InputStep&) const = default;
};

// How the chord triggers: Press (default; one step, or two as a sequence), Hold (active while held), Release (on key up), Double
// (second press within 300 ms).
enum class ChordMode { Press, Hold, Release, DoubleTap };

struct Chord {
    ChordMode mode = ChordMode::Press;
    std::vector<InputStep> steps;     // 1 step; 2 for a sequence (Press only)
    bool operator==(const Chord&) const = default;
};

// "Ctrl+Shift+S", "Ctrl+K, Ctrl+S", "Hold Space", "Release Alt", "Double Shift", "Shift+LMB", "Ctrl+RMB drag", "MMB drag", "Mouse4",
// "Wheel Up", "Ctrl+Wheel", "phys:W", "Numpad .". Names are case-insensitive. False with a message in *error on a bad chord.
bool parseChord(const std::string& text, Chord& out, std::string* error = nullptr);
// Canonical text of a chord (parseChord(formatChord(c)) == c).
std::string formatChord(const Chord& chord);
// Canonical key name for user text or a layout character ("a" -> "A", "esc" -> "Escape", "numpad5" -> "Numpad 5"); empty if unknown.
std::string canonicalKey(const std::string& name);

// What the router receives.
struct InputEvent {
    enum class Type { KeyPress, KeyRelease, MousePress, MouseRelease, MouseMove, Wheel };
    Type type = Type::KeyPress;
    uint8_t mods = 0;          // modifier keys held; the key's own modifier is not included (Shift press has mods 0)
    std::string key;           // canonical name of the key the active layout produced ("" if none)
    std::string scan;          // canonical name of the same physical key on a US layout ("Z" for the Cyrillic Я key)
    int button = 0;            // Mouse*: 1 LMB, 2 RMB, 3 MMB, 4 / 5
    double x = 0, y = 0;       // pointer position in the viewport, pixels
    double dx = 0, dy = 0;     // MouseMove: motion since the last event
    double wheel = 0;          // Wheel: ~100 per notch, positive = towards the user (down), as the Shell
    int64_t timeMs = 0;        // monotonic milliseconds (sequences time out after 1000, double taps after 300)
    bool autoRepeat = false;
    bool textFieldFocused = false;   // a text field has the keyboard: printable keys belong to it
    // Mouse events: the buttons the system says are down right now (bit n-1 = button n). With `buttonsKnown` the router ends a mouse hold whose
    // button is not in it any more (the release was lost: a popup took the mouse, focus moved away); events without it say nothing about buttons.
    uint8_t buttons = 0;
    bool buttonsKnown = false;
};

// Name of the key an event stands for, for hold bookkeeping ("W", "Shift").
std::string eventKeyId(const InputEvent& e);
// Does the event's key match this step's key (logical or physical)? Non-Latin layout characters fall back to the US position.
bool keyMatches(const InputStep& step, const InputEvent& e);
// Does this event press / click / scroll the step (modifiers equal unless ignoreShift)?
bool stepMatches(const InputStep& step, const InputEvent& e, bool ignoreShift);

} // namespace native::actions
