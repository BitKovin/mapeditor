// app/actions/registry -- see registry.h.
#include "app/actions/registry.h"

namespace native::actions {

bool Registry::add(ActionDef def) {
    if (def.id.empty() || index_.count(def.id) || aliases_.count(def.id)) return false;
    index_[def.id] = actions_.size();
    actions_.push_back(std::move(def));
    return true;
}

void Registry::addAlias(const std::string& oldId, const std::string& newId) {
    if (!oldId.empty() && !index_.count(oldId)) aliases_[oldId] = newId;
}

const ActionDef* Registry::find(const std::string& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) {
        auto al = aliases_.find(id);
        if (al == aliases_.end()) return nullptr;
        it = index_.find(al->second);
        if (it == index_.end()) return nullptr;
    }
    return &actions_[it->second];
}

bool Registry::isEnabled(const ActionDef& def, const ActionEnv& env, const nlohmann::json& params) const {
    return !def.enabled || def.enabled(env, params);
}

bool Registry::isChecked(const ActionDef& def, const ActionEnv& env, const nlohmann::json& params) const {
    return def.checked && def.checked(env, params);
}

bool Registry::run(const std::string& id, ActionEnv& env, const nlohmann::json& params) {
    const ActionDef* def = find(id);
    if (!def || !def->run) return false;
    const nlohmann::json p = params.is_null() ? def->params : params;
    if (!isEnabled(*def, env, p)) return false;
    Invocation inv;
    inv.params = p;
    def->run(env, inv);
    return true;
}

} // namespace native::actions
