// app/actions/router -- see router.h.
#include "app/actions/router.h"

#include <algorithm>
#include <cstdlib>

namespace native::actions {

namespace {

std::string triggerOf(const InputEvent& e) {
    const bool mouse = e.type == InputEvent::Type::MousePress || e.type == InputEvent::Type::MouseRelease;
    return mouse ? "btn:" + std::to_string(e.button) : "key:" + eventKeyId(e);
}

} // namespace

std::vector<std::string> Router::activeContexts() const {
    std::vector<std::string> list = contexts_.list();
    for (const Hold& h : holds_)
        if (!h.context.empty()) list.push_back(h.context);
    return list;
}

bool Router::contextActive(const std::string& name) const {
    const auto list = activeContexts();
    return std::find(list.begin(), list.end(), name) != list.end();
}

bool Router::available(const ActionDef& def) const {
    if (def.contexts.empty()) return true;
    return std::any_of(def.contexts.begin(), def.contexts.end(), [&](const std::string& c) { return contextActive(c); });
}

const ActionDef* Router::usable(const std::string& id, const nlohmann::json& params, const ActionEnv& env) const {
    const ActionDef* def = registry_.find(id);
    if (!def || !def->run || !available(*def)) return nullptr;
    const nlohmann::json p = params.is_null() ? def->params : params;
    return registry_.isEnabled(*def, env, p) ? def : nullptr;
}

bool Router::holding(const std::string& actionId) const {
    return std::any_of(holds_.begin(), holds_.end(), [&](const Hold& h) { return h.def->id == actionId; });
}

void Router::run(const ActionDef& def, const Binding& b, ActionEnv& env, const InputEvent& e) {
    Invocation inv;
    inv.event = e;
    inv.params = b.params.is_null() ? def.params : b.params;
    def.run(env, inv);
}

void Router::startHold(const ActionDef& def, const Binding& b, ActionEnv& env, const InputEvent& e) {
    Hold h;
    h.def = &def;
    h.params = b.params.is_null() ? def.params : b.params;
    h.trigger = triggerOf(e);
    h.context = def.holdContext;
    // A key held again (a second binding on the same key) replaces the old hold instead of stacking.
    for (size_t i = holds_.size(); i-- > 0;)
        if (holds_[i].trigger == h.trigger && holds_[i].def == &def) endHold(i, env, e);
    holds_.push_back(h);
    Invocation inv;
    inv.event = e;
    inv.params = h.params;
    def.run(env, inv);
}

void Router::endHold(size_t index, ActionEnv& env, const InputEvent& e) {
    const Hold h = holds_[index];
    holds_.erase(holds_.begin() + static_cast<std::ptrdiff_t>(index));
    InputEvent rel = e;
    if (rel.type != InputEvent::Type::KeyRelease && rel.type != InputEvent::Type::MouseRelease) rel.type = InputEvent::Type::KeyRelease;
    Invocation inv;
    inv.event = rel;
    inv.params = h.params;
    h.def->run(env, inv);
    // The holds that only exist inside this hold's context (the fly keys inside the look drag) end with it: nothing may stay "held" without its owner.
    if (!h.context.empty()) {
        for (size_t i = holds_.size(); i-- > 0;) {
            const auto& cs = holds_[i].def->contexts;
            if (std::find(cs.begin(), cs.end(), h.context) != cs.end()) endHold(i, env, e);
        }
    }
}

std::vector<std::string> Router::heldTriggers() const {
    std::vector<std::string> out;
    for (const Hold& h : holds_) out.push_back(h.trigger);
    return out;
}

bool Router::release(const std::string& trigger, ActionEnv& env, int64_t timeMs) {
    bool any = false;
    InputEvent e;
    e.type = trigger.rfind("btn:", 0) == 0 ? InputEvent::Type::MouseRelease : InputEvent::Type::KeyRelease;
    e.timeMs = timeMs;
    for (size_t i = holds_.size(); i-- > 0;) {
        if (i >= holds_.size() || holds_[i].trigger != trigger) continue;
        endHold(i, env, e);
        any = true;
    }
    return any;
}

void Router::cancelAll(ActionEnv& env, int64_t timeMs) {
    InputEvent e;
    e.type = InputEvent::Type::KeyRelease;
    e.timeMs = timeMs;
    while (!holds_.empty()) endHold(holds_.size() - 1, env, e);
    seqActive_ = false;
    haveTap_ = false;
}

std::string Router::resolve(const InputEvent& event, const ActionEnv& env) const {
    const auto& all = keymap_.effective();
    const std::vector<std::string> stack = activeContexts();
    for (size_t level = stack.size(); level-- > 0;)
        for (const Binding& b : all) {
            if (b.context != stack[level] || b.chord.steps.size() != 1) continue;
            const bool release = b.chord.mode == ChordMode::Release;
            if (release != (event.type == InputEvent::Type::KeyRelease || event.type == InputEvent::Type::MouseRelease)) continue;
            if (!stepMatches(b.chord.steps[0], event, b.ignoreShift)) continue;
            if (usable(b.action, b.params, env)) return b.action;
        }
    return {};
}

Router::Result Router::handle(const InputEvent& e, ActionEnv& env) {
    Result result;
    const bool keyEvent = e.type == InputEvent::Type::KeyPress || e.type == InputEvent::Type::KeyRelease;
    // A focused text field gets every key without Ctrl / Alt / Meta (typing); chords with those still reach the router.
    // A key RELEASE always counts: it may end a hold that began before the field got the keyboard.
    if (e.type != InputEvent::Type::KeyRelease && keyEvent && e.textFieldFocused && (e.mods & (kCtrl | kAlt | kMeta)) == 0) return result;

    if (e.type == InputEvent::Type::MouseMove) {
        if (e.buttonsKnown) {   // a mouse hold whose button the system no longer reports as down lost its release: end it now
            for (size_t i = holds_.size(); i-- > 0;) {
                if (i >= holds_.size() || holds_[i].trigger.rfind("btn:", 0) != 0) continue;
                const int button = std::atoi(holds_[i].trigger.c_str() + 4);
                if (button >= 1 && button <= 8 && (e.buttons & (1u << (button - 1))) == 0) {
                    InputEvent rel = e;
                    rel.type = InputEvent::Type::MouseRelease;
                    rel.button = button;
                    result.action = holds_[i].def->id;
                    result.handled = true;
                    endHold(i, env, rel);
                }
            }
        }
        // iterate by index: a hold's action may end holds
        for (size_t i = 0; i < holds_.size(); ++i) {
            if (holds_[i].trigger.rfind("btn:", 0) != 0) continue;
            Invocation inv;
            inv.event = e;
            inv.params = holds_[i].params;
            const ActionDef* def = holds_[i].def;
            def->run(env, inv);
            result.handled = true;
            result.action = def->id;
        }
        return result;
    }

    if (e.type == InputEvent::Type::KeyRelease || e.type == InputEvent::Type::MouseRelease) {
        const std::string trig = triggerOf(e);
        for (size_t i = holds_.size(); i-- > 0;)
            if (holds_[i].trigger == trig) {
                result.action = holds_[i].def->id;
                result.handled = true;
                endHold(i, env, e);
            }
        if (result.handled) return result;
        // Release-mode bindings.
        const std::vector<std::string> stack = activeContexts();
        const auto& all = keymap_.effective();
        for (size_t level = stack.size(); level-- > 0;)
            for (const Binding& b : all) {
                if (b.context != stack[level] || b.chord.mode != ChordMode::Release || b.chord.steps.size() != 1) continue;
                if (!stepMatches(b.chord.steps[0], e, true)) continue;
                if (const ActionDef* def = usable(b.action, b.params, env)) {
                    run(*def, b, env, e);
                    result.handled = true;
                    result.action = def->id;
                    return result;
                }
            }
        return result;
    }

    // ---- presses, clicks and wheel ----
    if (e.type == InputEvent::Type::KeyPress && e.autoRepeat) {
        // Key repeat never starts holds, sequences or double taps; a held key's own repeats are swallowed.
        if (std::any_of(holds_.begin(), holds_.end(), [&](const Hold& h) { return h.trigger == triggerOf(e); })) {
            result.handled = true;
            return result;
        }
    }
    const bool repeat = e.type == InputEvent::Type::KeyPress && e.autoRepeat;

    if (seqActive_ && e.timeMs - seqFirst_.timeMs > kSequenceTimeoutMs) seqActive_ = false;
    const auto& all = keymap_.effective();
    const std::vector<std::string> stack = activeContexts();

    if (seqActive_ && keyEvent) {
        for (size_t level = stack.size(); level-- > 0;)
            for (const Binding& b : all) {
                if (b.context != stack[level] || b.chord.steps.size() != 2) continue;
                if (!stepMatches(b.chord.steps[0], seqFirst_, b.ignoreShift) || !stepMatches(b.chord.steps[1], e, b.ignoreShift)) continue;
                if (const ActionDef* def = usable(b.action, b.params, env)) {
                    seqActive_ = false;
                    run(*def, b, env, e);
                    result.handled = true;
                    result.action = def->id;
                    return result;
                }
            }
        seqActive_ = false;   // not a continuation: the event is handled on its own below
    } else {
        seqActive_ = false;
    }

    for (size_t level = stack.size(); level-- > 0;) {
        bool prefix = false;
        for (const Binding& b : all) {
            if (b.context != stack[level] || b.chord.mode == ChordMode::Release) continue;
            const InputStep& s0 = b.chord.steps[0];
            if (!stepMatches(s0, e, b.ignoreShift)) continue;
            const ActionDef* def = usable(b.action, b.params, env);
            if (!def) continue;
            if (b.chord.steps.size() == 2) {
                if (!repeat && keyEvent) prefix = true;
                continue;
            }
            switch (b.chord.mode) {
                case ChordMode::Press:
                    run(*def, b, env, e);
                    break;
                case ChordMode::Hold:
                    if (repeat || e.type == InputEvent::Type::Wheel) continue;
                    startHold(*def, b, env, e);
                    break;
                case ChordMode::DoubleTap:
                    if (repeat || !haveTap_ || tapId_ != triggerOf(e) || tapMods_ != e.mods || e.timeMs - tapTime_ > kDoubleTapMs) continue;
                    haveTap_ = false;
                    run(*def, b, env, e);
                    break;
                case ChordMode::Release:
                    continue;
            }
            result.handled = true;
            result.action = def->id;
            if (e.type != InputEvent::Type::Wheel && !repeat) {
                haveTap_ = b.chord.mode != ChordMode::DoubleTap;
                tapId_ = triggerOf(e);
                tapMods_ = e.mods;
                tapTime_ = e.timeMs;
            }
            return result;
        }
        if (prefix) {
            seqActive_ = true;
            seqFirst_ = e;
            result.handled = true;
            return result;
        }
    }
    if (!repeat && e.type != InputEvent::Type::Wheel) {
        haveTap_ = true;
        tapId_ = triggerOf(e);
        tapMods_ = e.mods;
        tapTime_ = e.timeMs;
    }
    return result;
}

} // namespace native::actions
