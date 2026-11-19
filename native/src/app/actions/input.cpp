// app/actions/input -- see input.h.
#include "app/actions/input.h"

#include <algorithm>
#include <cctype>

namespace native::actions {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool startsWithNoCase(const std::string& s, const char* prefix) {
    const std::string p = lower(prefix);
    return lower(s.substr(0, p.size())) == p;
}

bool isNonLatin(const std::string& key) { return !key.empty() && (static_cast<unsigned char>(key[0]) >= 0x80); }

bool setError(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

// Parses "Ctrl+Shift+" prefixes then the rest.
bool parseStep(const std::string& textIn, InputStep& out, std::string* error) {
    std::string text = trim(textIn);
    out = InputStep{};
    for (;;) {
        bool eaten = false;
        const struct { const char* name; uint8_t bit; } mods[] = {{"ctrl+", kCtrl}, {"control+", kCtrl}, {"shift+", kShift},
                                                                {"alt+", kAlt}, {"meta+", kMeta}, {"cmd+", kMeta}, {"win+", kMeta}};
        for (const auto& m : mods)
            if (startsWithNoCase(text, m.name) && text.size() > std::string(m.name).size()) {
                out.mods |= m.bit;
                text = text.substr(std::string(m.name).size());
                eaten = true;
                break;
            }
        if (!eaten) break;
    }
    const std::string low = lower(text);
    if (low == "lmb" || low == "rmb" || low == "mmb" || low == "mouse4" || low == "mouse5" || low == "lmb drag" || low == "rmb drag" ||
        low == "mmb drag" || low == "mouse4 drag" || low == "mouse5 drag") {
        out.type = InputStep::Type::Mouse;
        out.button = low[0] == 'l' ? 1 : (low[0] == 'r' ? 2 : (low[0] == 'm' && low[1] == 'm' ? 3 : (low[5] == '4' ? 4 : 5)));
        out.drag = low.size() > 5 && low.find("drag") != std::string::npos;
        return true;
    }
    if (low == "wheel" || low == "wheel up" || low == "wheel down") {
        out.type = InputStep::Type::Wheel;
        out.wheelDir = low == "wheel up" ? 1 : (low == "wheel down" ? -1 : 0);
        return true;
    }
    if (startsWithNoCase(text, "phys:")) {
        out.physical = true;
        text = text.substr(5);
    }
    const std::string key = canonicalKey(text);
    if (key.empty()) return setError(error, "unknown key \"" + text + "\"");
    out.key = key;
    return true;
}

std::string stepText(const InputStep& s) {
    std::string t;
    if (s.mods & kCtrl) t += "Ctrl+";
    if (s.mods & kShift) t += "Shift+";
    if (s.mods & kAlt) t += "Alt+";
    if (s.mods & kMeta) t += "Meta+";
    switch (s.type) {
        case InputStep::Type::Key: t += (s.physical ? "phys:" : "") + s.key; break;
        case InputStep::Type::Mouse: {
            static const char* names[] = {"", "LMB", "RMB", "MMB", "Mouse4", "Mouse5"};
            t += names[std::clamp(s.button, 0, 5)];
            if (s.drag) t += " drag";
            break;
        }
        case InputStep::Type::Wheel: t += s.wheelDir > 0 ? "Wheel Up" : (s.wheelDir < 0 ? "Wheel Down" : "Wheel"); break;
    }
    return t;
}

} // namespace

std::string canonicalKey(const std::string& nameIn) {
    const std::string name = trim(nameIn);
    if (name.empty()) return {};
    const std::string low = lower(name);
    if (name.size() == 1) {
        const char c = name[0];
        if (std::isalpha(static_cast<unsigned char>(c))) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (std::isdigit(static_cast<unsigned char>(c))) return name;
        if (std::string("`-=[]\\;',./").find(c) != std::string::npos) return name;
        return {};
    }
    if (isNonLatin(name)) return name;   // a layout character (kept as is; matching falls back to the US position)
    static const struct { const char* alias; const char* canon; } names[] = {
        {"space", "Space"}, {"tab", "Tab"}, {"enter", "Enter"}, {"return", "Enter"}, {"escape", "Escape"}, {"esc", "Escape"},
        {"backspace", "Backspace"}, {"delete", "Delete"}, {"del", "Delete"}, {"insert", "Insert"}, {"ins", "Insert"}, {"home", "Home"},
        {"end", "End"}, {"pageup", "PageUp"}, {"pgup", "PageUp"}, {"pagedown", "PageDown"}, {"pgdn", "PageDown"}, {"left", "Left"},
        {"right", "Right"}, {"up", "Up"}, {"down", "Down"}, {"shift", "Shift"}, {"ctrl", "Ctrl"}, {"control", "Ctrl"}, {"alt", "Alt"},
        {"meta", "Meta"}, {"capslock", "CapsLock"}, {"numpad .", "Numpad ."}, {"numpad+", "Numpad +"}, {"numpad +", "Numpad +"},
        {"numpad-", "Numpad -"}, {"numpad -", "Numpad -"}, {"numpad*", "Numpad *"}, {"numpad *", "Numpad *"}, {"numpad/", "Numpad /"},
        {"numpad /", "Numpad /"}, {"numpad.", "Numpad ."}, {"numpadenter", "Numpad Enter"}, {"numpad enter", "Numpad Enter"}};
    for (const auto& n : names)
        if (low == n.alias) return n.canon;
    if (low[0] == 'f' && low.size() <= 3 && std::all_of(low.begin() + 1, low.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const int n = std::atoi(low.c_str() + 1);
        if (n >= 1 && n <= 24) return "F" + std::to_string(n);
    }
    if (startsWithNoCase(low, "numpad")) {
        std::string rest = trim(low.substr(6));
        if (rest.size() == 1 && std::isdigit(static_cast<unsigned char>(rest[0]))) return "Numpad " + rest;
    }
    return {};
}

bool parseChord(const std::string& textIn, Chord& out, std::string* error) {
    out = Chord{};
    std::string text = trim(textIn);
    if (text.empty()) return setError(error, "empty chord");
    if (startsWithNoCase(text, "hold ")) { out.mode = ChordMode::Hold; text = trim(text.substr(5)); }
    else if (startsWithNoCase(text, "release ")) { out.mode = ChordMode::Release; text = trim(text.substr(8)); }
    else if (startsWithNoCase(text, "double ")) { out.mode = ChordMode::DoubleTap; text = trim(text.substr(7)); }
    // Steps are separated by ", " (a comma followed by a space: a bare "," is the comma key).
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(", ", from);
        if (at == std::string::npos) { parts.push_back(text.substr(from)); break; }
        parts.push_back(text.substr(from, at - from));
        from = at + 2;
    }
    if (parts.size() > 2) return setError(error, "a sequence has at most 2 steps");
    if (parts.size() == 2 && out.mode != ChordMode::Press) return setError(error, "only a plain press can be a sequence");
    for (const std::string& p : parts) {
        InputStep s;
        if (!parseStep(p, s, error)) return false;
        out.steps.push_back(s);
    }
    const InputStep& first = out.steps[0];
    if (out.mode == ChordMode::Release && first.type != InputStep::Type::Key && first.type != InputStep::Type::Mouse)
        return setError(error, "Release needs a key or mouse button");
    if (out.mode == ChordMode::Hold && first.type == InputStep::Type::Wheel) return setError(error, "a wheel cannot be held");
    if (first.type == InputStep::Type::Mouse && first.drag && out.mode == ChordMode::Press) out.mode = ChordMode::Hold;   // a drag is a hold
    return true;
}

std::string formatChord(const Chord& c) {
    std::string t;
    const bool dragHold = c.mode == ChordMode::Hold && c.steps.size() == 1 && c.steps[0].type == InputStep::Type::Mouse && c.steps[0].drag;
    if (!dragHold) {
        if (c.mode == ChordMode::Hold) t = "Hold ";
        else if (c.mode == ChordMode::Release) t = "Release ";
        else if (c.mode == ChordMode::DoubleTap) t = "Double ";
    }
    for (size_t i = 0; i < c.steps.size(); ++i) t += (i ? ", " : "") + stepText(c.steps[i]);
    return t;
}

std::string eventKeyId(const InputEvent& e) { return !e.scan.empty() ? e.scan : e.key; }

bool keyMatches(const InputStep& step, const InputEvent& e) {
    if (step.type != InputStep::Type::Key) return false;
    if (step.physical) return !e.scan.empty() ? e.scan == step.key : e.key == step.key;
    if (!e.key.empty() && !isNonLatin(e.key)) return e.key == step.key;
    // A non-Latin layout character (or none): the key's position on the US layout stands in for it.
    return !e.scan.empty() && e.scan == step.key;
}

bool stepMatches(const InputStep& step, const InputEvent& e, bool ignoreShift) {
    // Exact modifiers; with ignoreShift an extra Shift is tolerated (flying with Shift held = boost), never Ctrl / Alt / Meta.
    if (ignoreShift) {
        if ((step.mods & ~e.mods) != 0 || (e.mods & ~step.mods & ~kShift) != 0) return false;
    } else if (step.mods != e.mods) {
        return false;
    }
    switch (step.type) {
        case InputStep::Type::Key:
            return (e.type == InputEvent::Type::KeyPress || e.type == InputEvent::Type::KeyRelease) && keyMatches(step, e);
        case InputStep::Type::Mouse:
            return (e.type == InputEvent::Type::MousePress || e.type == InputEvent::Type::MouseRelease) && e.button == step.button;
        case InputStep::Type::Wheel:
            return e.type == InputEvent::Type::Wheel && (step.wheelDir == 0 || (step.wheelDir > 0 ? e.wheel < 0 : e.wheel > 0));
    }
    return false;
}

} // namespace native::actions
