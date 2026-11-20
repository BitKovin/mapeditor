// app/actions/keymap -- see keymap.h.
#include "app/actions/keymap.h"

#include <algorithm>

namespace native::actions {

namespace {

std::string fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return msg;
}

bool readBindings(const nlohmann::json& arr, bool fromUser, std::vector<Binding>& out, std::string* error, const char* what) {
    if (!arr.is_array()) {
        fail(error, std::string(what) + " must be an array");
        return false;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& j = arr[i];
        const std::string at = std::string(what) + "[" + std::to_string(i) + "]";
        if (!j.is_object() || !j.contains("action") || !j["action"].is_string() || !j.contains("input") || !j["input"].is_string()) {
            fail(error, at + ": needs \"action\" and \"input\"");
            return false;
        }
        const std::string context = j.contains("context") && j["context"].is_string() ? j["context"].get<std::string>() : "global";
        const nlohmann::json params = j.contains("params") ? j["params"] : nlohmann::json(nullptr);
        const bool ignoreShift = j.contains("ignoreShift") && j["ignoreShift"].is_boolean() && j["ignoreShift"].get<bool>();
        Binding b;
        std::string err;
        if (!makeBinding(j["action"].get<std::string>(), context, j["input"].get<std::string>(), params, ignoreShift, b, &err)) {
            fail(error, at + ": " + err);
            return false;
        }
        b.fromUser = fromUser;
        out.push_back(std::move(b));
    }
    return true;
}

nlohmann::json bindingJson(const Binding& b) {
    nlohmann::json j = {{"action", b.action}, {"context", b.context}, {"input", b.input}};
    if (!b.params.is_null()) j["params"] = b.params;
    if (b.ignoreShift) j["ignoreShift"] = true;
    return j;
}

} // namespace

bool makeBinding(const std::string& action, const std::string& context, const std::string& input, const nlohmann::json& params,
                 bool ignoreShift, Binding& out, std::string* error) {
    Chord chord;
    std::string err;
    if (action.empty()) {
        if (error) *error = "empty action id";
        return false;
    }
    if (!parseChord(input, chord, &err)) {
        if (error) *error = "bad input \"" + input + "\": " + err;
        return false;
    }
    out = Binding{};
    out.action = action;
    out.context = context.empty() ? "global" : context;
    out.chord = chord;
    out.input = formatChord(chord);
    out.params = params;
    out.ignoreShift = ignoreShift;
    return true;
}

bool Keymap::loadPreset(const std::string& jsonText, std::string* error) {
    const nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
    if (!j.is_object()) {
        fail(error, "preset is not a JSON object");
        return false;
    }
    std::vector<Binding> list;
    if (!readBindings(j.contains("bindings") ? j["bindings"] : nlohmann::json::array(), false, list, error, "bindings")) return false;
    presetName_ = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : "preset";
    preset_ = std::move(list);
    dirty_ = true;
    return true;
}

bool Keymap::loadUser(const std::string& jsonText, std::string* error) {
    const nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
    if (!j.is_object()) {
        fail(error, "user keymap is not a JSON object");
        return false;
    }
    std::vector<Binding> bind, unbind;
    if (!readBindings(j.contains("bind") ? j["bind"] : nlohmann::json::array(), true, bind, error, "bind")) return false;
    if (!readBindings(j.contains("unbind") ? j["unbind"] : nlohmann::json::array(), true, unbind, error, "unbind")) return false;
    userBind_ = std::move(bind);
    userUnbind_ = std::move(unbind);
    dirty_ = true;
    return true;
}

std::string Keymap::userToJson() const {
    nlohmann::json j;
    j["base"] = presetName_;
    j["version"] = 1;
    j["bind"] = nlohmann::json::array();
    j["unbind"] = nlohmann::json::array();
    for (const Binding& b : userBind_) j["bind"].push_back(bindingJson(b));
    for (const Binding& b : userUnbind_) j["unbind"].push_back(bindingJson(b));
    return j.dump(2);
}

void Keymap::bindUser(const Binding& b) {
    Binding nb = b;
    nb.fromUser = true;
    // Binding something that was removed from the preset brings it back instead of adding a duplicate.
    userUnbind_.erase(std::remove_if(userUnbind_.begin(), userUnbind_.end(), [&](const Binding& u) { return u.sameAs(nb); }), userUnbind_.end());
    const bool inPreset = std::any_of(preset_.begin(), preset_.end(), [&](const Binding& p) { return p.sameAs(nb); });
    userBind_.erase(std::remove_if(userBind_.begin(), userBind_.end(), [&](const Binding& u) { return u.sameAs(nb); }), userBind_.end());
    if (!inPreset) userBind_.push_back(nb);
    dirty_ = true;
}

bool Keymap::unbindUser(const Binding& b) {
    auto it = std::find_if(userBind_.begin(), userBind_.end(), [&](const Binding& u) { return u.sameAs(b); });
    if (it != userBind_.end()) {
        userBind_.erase(it);
        dirty_ = true;
        return true;
    }
    const bool inPreset = std::any_of(preset_.begin(), preset_.end(), [&](const Binding& p) { return p.sameAs(b); });
    const bool already = std::any_of(userUnbind_.begin(), userUnbind_.end(), [&](const Binding& u) { return u.sameAs(b); });
    if (!inPreset || already) return false;
    Binding ub = b;
    ub.fromUser = true;
    userUnbind_.push_back(ub);
    dirty_ = true;
    return true;
}

void Keymap::clearUser() {
    userBind_.clear();
    userUnbind_.clear();
    dirty_ = true;
}

bool Keymap::resetUserAction(const std::string& action) {
    const size_t before = userBind_.size() + userUnbind_.size();
    const auto hidesPreset = [&](const Binding& u) {
        return std::any_of(preset_.begin(), preset_.end(), [&](const Binding& p) { return p.action == action && p.context == u.context && p.input == u.input; });
    };
    userBind_.erase(std::remove_if(userBind_.begin(), userBind_.end(), [&](const Binding& u) { return u.action == action || hidesPreset(u); }), userBind_.end());
    userUnbind_.erase(std::remove_if(userUnbind_.begin(), userUnbind_.end(), [&](const Binding& u) { return u.action == action; }), userUnbind_.end());
    const bool changed = userBind_.size() + userUnbind_.size() != before;
    if (changed) dirty_ = true;
    return changed;
}

void Keymap::rebuild() const {
    effective_.clear();
    for (const Binding& p : preset_) {
        const bool removed = std::any_of(userUnbind_.begin(), userUnbind_.end(), [&](const Binding& u) { return u.sameAs(p); });
        // A user binding of the same chord in the same context takes the input over (one action per input per context).
        const bool replaced = std::any_of(userBind_.begin(), userBind_.end(),
                                          [&](const Binding& u) { return u.context == p.context && u.input == p.input; });
        if (!removed && !replaced) effective_.push_back(p);
    }
    for (const Binding& u : userBind_) effective_.push_back(u);
    dirty_ = false;
}

const std::vector<Binding>& Keymap::effective() const {
    if (dirty_) rebuild();
    return effective_;
}

std::vector<const Binding*> Keymap::bindingsFor(const std::string& action) const {
    std::vector<const Binding*> out;
    for (const Binding& b : effective())
        if (b.action == action) out.push_back(&b);
    return out;
}

std::string Keymap::displayText(const std::string& action, const nlohmann::json& params) const {
    for (const Binding& b : effective()) {
        if (b.action != action) continue;
        if (!params.is_null() && b.params != params) continue;
        if (b.chord.mode == ChordMode::Press && b.chord.steps[0].type == InputStep::Type::Key) return b.input;
    }
    return {};
}

std::vector<std::pair<const Binding*, const Binding*>> Keymap::conflicts() const {
    std::vector<std::pair<const Binding*, const Binding*>> out;
    const auto& e = effective();
    for (size_t i = 0; i < e.size(); ++i)
        for (size_t j = i + 1; j < e.size(); ++j)
            if (e[i].context == e[j].context && e[i].input == e[j].input && !(e[i].action == e[j].action && e[i].params == e[j].params))
                out.emplace_back(&e[i], &e[j]);
    return out;
}

} // namespace native::actions
