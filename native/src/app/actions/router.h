// app/actions/router -- the one input router: turns an input event into an action.
//
// Owns: resolution over the context stack (most specific context first; the first binding whose action is available and enabled
// wins), key sequences (2 steps, 1 s timeout), Hold / Release / Double-tap chords, mouse and wheel chords, the active holds
// (a held mouse drag or key keeps its action running and its hold context on the stack until it is released), logical and
// physical keys (non-Latin layouts are matched by US key position) and the text-field rule (a focused text field gets printable
// keys; only chords with Ctrl / Alt / Meta reach the router). Qt-free and clock-free: events carry their own timestamps.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §3 and §4, 08_MILESTONES.md T1.6.
#pragma once

#include <string>
#include <vector>

#include "app/actions/contexts.h"
#include "app/actions/keymap.h"
#include "app/actions/registry.h"

namespace native::actions {

constexpr int64_t kSequenceTimeoutMs = 1000;
constexpr int64_t kDoubleTapMs = 300;

class Router {
public:
    // The registry and keymap must outlive the router; the keymap may change between events (the router reads it each time).
    Router(const Registry& registry, const Keymap& keymap) : registry_(registry), keymap_(keymap) {}

    // The contexts the application says are active (window, area, view, mode, tool, modal). The router adds the hold contexts of
    // its running holds on top of these; setting the base never disturbs a hold.
    ContextStack& contexts() { return contexts_; }
    const ContextStack& contexts() const { return contexts_; }
    // Base contexts followed by the hold contexts of the running holds (outermost first).
    std::vector<std::string> activeContexts() const;
    bool contextActive(const std::string& name) const;

    struct Result {
        bool handled = false;       // the event was consumed (the caller must not pass it on)
        std::string action;         // the action that ran or started; empty when only a sequence prefix / a move was consumed
    };
    Result handle(const InputEvent& event, ActionEnv& env);

    // The action `event` would run in the current contexts, without running anything (reverse lookup, key help, tests).
    std::string resolve(const InputEvent& event, const ActionEnv& env) const;

    bool holding() const { return !holds_.empty(); }
    bool holding(const std::string& actionId) const;
    bool sequencePending() const { return seqActive_; }
    // Ends every hold (focus lost, window deactivated) and forgets a pending sequence.
    void cancelAll(ActionEnv& env, int64_t timeMs = 0);
    // The triggers of the running holds: "btn:2" (a mouse button), "key:W" (a key by its event key id).
    std::vector<std::string> heldTriggers() const;
    // Ends the holds started by `trigger` as if it had been released (a release the system never delivered). True when one ended.
    bool release(const std::string& trigger, ActionEnv& env, int64_t timeMs = 0);
    static std::string mouseTrigger(int button) { return "btn:" + std::to_string(button); }

private:
    struct Hold {
        const ActionDef* def = nullptr;
        nlohmann::json params;
        std::string trigger;        // "btn:2" or "key:W"
        std::string context;        // pushed hold context ("" when none)
    };
    const ActionDef* usable(const std::string& id, const nlohmann::json& params, const ActionEnv& env) const;
    bool available(const ActionDef& def) const;
    void run(const ActionDef& def, const Binding& b, ActionEnv& env, const InputEvent& e);
    void startHold(const ActionDef& def, const Binding& b, ActionEnv& env, const InputEvent& e);
    void endHold(size_t index, ActionEnv& env, const InputEvent& e);

    const Registry& registry_;
    const Keymap& keymap_;
    ContextStack contexts_;           // set by the application
    std::vector<Hold> holds_;
    bool seqActive_ = false;
    InputEvent seqFirst_;
    bool haveTap_ = false;
    std::string tapId_;
    uint8_t tapMods_ = 0;
    int64_t tapTime_ = 0;
};

} // namespace native::actions
