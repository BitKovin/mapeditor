// app/actions/keymap -- bindings and keymap layers.
//
// Owns: Binding (action + context + chord [+ parameters]), the built-in preset (read-only, JSON), the user keymap (only the
// differences: added and removed bindings, JSON) and the effective keymap = preset minus removals plus additions. An input can be
// bound to one action per context: a user binding replaces a preset binding of the same chord in the same context.
// Qt-free; files are read / written by the caller (so the format is testable without a disk).
// Spec: 10_UI_AND_INPUT.md §4.2, 08_MILESTONES.md T1.6.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

#include "app/actions/input.h"

namespace native::actions {

struct Binding {
    std::string action;
    std::string context = "global";     // the context that must be active ("area:viewport", "hold:camera.fly" ...)
    std::string input;                  // canonical chord text
    Chord chord;
    nlohmann::json params;              // null = the action's default parameters
    bool ignoreShift = false;               // an extra Shift does not stop it matching (fly keys while Shift boosts); Ctrl / Alt / Meta still do
    bool fromUser = false;

    // Same action, context, chord and parameters.
    bool sameAs(const Binding& o) const { return action == o.action && context == o.context && input == o.input && params == o.params; }
};

// Builds a binding from text; false with *error when the chord does not parse.
bool makeBinding(const std::string& action, const std::string& context, const std::string& input, const nlohmann::json& params,
                 bool ignoreShift, Binding& out, std::string* error = nullptr);

class Keymap {
public:
    // Preset JSON: { "name": "default", "bindings": [ { "action", "context", "input", "params"?, "ignoreShift"? } ] }.
    // False (the keymap is left unchanged) on malformed JSON or a bad chord; *error says which entry.
    bool loadPreset(const std::string& jsonText, std::string* error = nullptr);
    // User JSON: { "base": "default", "version": 1, "bind": [...], "unbind": [...] }. Replaces the previous user layer.
    bool loadUser(const std::string& jsonText, std::string* error = nullptr);
    std::string userToJson() const;
    const std::string& presetName() const { return presetName_; }

    // Editing the user layer (the binding editor of T4.7 calls these).
    void bindUser(const Binding& b);
    // Removes the binding from the effective keymap: drops it from the user layer if it was added there, otherwise records it as
    // removed from the preset. False when no such binding is effective.
    bool unbindUser(const Binding& b);
    void clearUser();
    bool userEmpty() const { return userBind_.empty() && userUnbind_.empty(); }

    // Preset minus removals plus additions, in a stable order (preset order, then additions).
    const std::vector<Binding>& effective() const;
    std::vector<const Binding*> bindingsFor(const std::string& action) const;
    // "Ctrl+Z" for menus: the first effective binding of the action (and params, when given) that is a plain press; empty if none.
    std::string displayText(const std::string& action, const nlohmann::json& params = nullptr) const;
    // Pairs of effective bindings with the same chord in the same context (a preset / user mistake the editor flags).
    std::vector<std::pair<const Binding*, const Binding*>> conflicts() const;

private:
    void rebuild() const;

    std::string presetName_;
    std::vector<Binding> preset_;
    std::vector<Binding> userBind_;
    std::vector<Binding> userUnbind_;
    mutable std::vector<Binding> effective_;
    mutable bool dirty_ = true;
};

} // namespace native::actions
