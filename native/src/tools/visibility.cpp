// tools/visibility -- see visibility.h.
#include "tools/visibility.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>

namespace native::tools {

std::string entityKey(const std::string& id) { return "e:" + id; }
std::string instanceKey(const std::string& id) { return "i:" + id; }
std::string brushKey(const std::string& entityId, const std::string& brushId) { return "b:" + entityId + "|" + brushId; }

std::vector<HideRule> defaultHideRules() {
    std::vector<HideRule> r(2);
    r[0].pattern = "trigger_*";
    r[1].pattern = "editor_playable_area";
    return r;
}

std::string normalizePattern(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isspace(c)) continue;
        const char l = static_cast<char>(std::tolower(c));
        if (l == '*' && !out.empty() && out.back() == '*') continue;
        out += l;
    }
    return out;
}

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// `*` matches any run; everything else is a plain character (the usual two-pointer wildcard match, no regex library).
bool wildcard(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

} // namespace

std::function<bool(const std::string&)> ruleMatcher(const std::vector<HideRule>& rules) {
    std::vector<std::string> on;
    for (const HideRule& r : rules)
        if (r.enabled && !normalizePattern(r.pattern).empty()) on.push_back(normalizePattern(r.pattern));
    if (on.empty()) return [](const std::string&) { return false; };
    return [on](const std::string& classname) {
        const std::string n = lower(classname);
        if (n == "worldspawn") return false;
        return std::any_of(on.begin(), on.end(), [&](const std::string& p) { return wildcard(p, n); });
    };
}

std::vector<HideRule> restoreHideRules(const nlohmann::json& raw) {
    std::vector<HideRule> rules = defaultHideRules();
    if (!raw.is_array()) return rules;
    for (const auto& x : raw) {
        if (!x.is_object() || !x.contains("pattern") || !x["pattern"].is_string() || !x.contains("enabled") || !x["enabled"].is_boolean()) continue;
        const std::string p = normalizePattern(x["pattern"].get<std::string>());
        if (p.empty() || p.size() > 80) continue;
        const auto own = std::find_if(rules.begin(), rules.end(), [&](const HideRule& r) { return normalizePattern(r.pattern) == p; });
        if (own != rules.end()) own->enabled = x["enabled"].get<bool>();
        else rules.push_back({p, x["enabled"].get<bool>(), true});
    }
    return rules;
}

nlohmann::json hideRulesToJson(const std::vector<HideRule>& rules) {
    nlohmann::json a = nlohmann::json::array();
    for (const HideRule& r : rules) a.push_back({{"pattern", r.pattern}, {"enabled", r.enabled}});
    return a;
}

const HideRule* matchingRule(const std::string& classname, const std::vector<HideRule>& rules) {
    for (const HideRule& r : rules)
        if (ruleMatcher({r})(classname)) return &r;
    return nullptr;
}

std::vector<HideRule> addRule(const std::vector<HideRule>& rules, const std::string& pattern) {
    const std::string p = normalizePattern(pattern);
    std::vector<HideRule> out = rules;
    if (p.empty()) return out;
    for (HideRule& r : out)
        if (normalizePattern(r.pattern) == p) {
            r.enabled = true;
            return out;
        }
    out.push_back({p, true, true});
    return out;
}

std::vector<VolumeClass> volumeClasses(const doc::DocumentVersion& d) {
    std::map<std::string, int> n;
    for (const auto& e : d.entities) {
        const std::string& c = e->typed.classname;
        if (c.empty() || c == "worldspawn" || e->brushes.empty()) continue;
        ++n[c];
    }
    std::vector<VolumeClass> out;
    for (const auto& [c, k] : n) out.push_back({c, k});
    const auto rank = [](const std::string& c) { return c.rfind("trigger_", 0) == 0 ? 0 : (c.rfind("editor_", 0) == 0 ? 1 : 2); };
    std::stable_sort(out.begin(), out.end(), [&](const VolumeClass& a, const VolumeClass& b) { return rank(a.classname) != rank(b.classname) ? rank(a.classname) < rank(b.classname) : a.classname < b.classname; });
    return out;
}

bool HideState::any() const {
    return !keys.empty() || std::any_of(rules.begin(), rules.end(), [](const HideRule& r) { return r.enabled; });
}
bool HideState::entityHidden(const std::string& entityId, const std::string& classname) const {
    return keys.count(entityKey(entityId)) || ruleMatcher(rules)(classname);
}
bool HideState::brushHidden(const std::string& entityId, const std::string& brushId, const std::string& classname) const {
    return entityHidden(entityId, classname) || keys.count(brushKey(entityId, brushId));
}
bool HideState::instanceHidden(const std::string& instanceId) const { return keys.count(instanceKey(instanceId)) > 0; }

std::vector<std::string> hideKeys(const std::vector<doc::SelRef>& sels) {
    std::vector<std::string> out;
    for (const doc::SelRef& s : sels) {
        if (s.isInstance()) {
            out.push_back(instanceKey(s.instancePath[0]));
            continue;
        }
        if (!s.instancePath.empty() || s.entityId.empty()) continue;
        out.push_back(s.brushId.empty() ? entityKey(s.entityId) : brushKey(s.entityId, s.brushId));
    }
    return out;
}

std::vector<doc::SelRef> pruneHidden(const doc::DocumentVersion& d, const std::vector<doc::SelRef>& sels, const HideState& hide) {
    const auto matcher = ruleMatcher(hide.rules);
    std::vector<doc::SelRef> keep;
    for (const doc::SelRef& s : sels) {
        bool gone = false;
        if (!s.instancePath.empty()) {
            gone = hide.instanceHidden(s.instancePath[0]);
        } else if (!s.entityId.empty()) {
            const int ei = d.findEntity(s.entityId);
            const std::string cls = ei >= 0 ? d.entities[static_cast<size_t>(ei)]->typed.classname : std::string();
            gone = hide.keys.count(entityKey(s.entityId)) || (!s.brushId.empty() && hide.keys.count(brushKey(s.entityId, s.brushId))) || (ei >= 0 && matcher(cls));
        }
        if (!gone) keep.push_back(s);
    }
    return keep;
}

std::vector<VisibleEntity> visibleEntities(const doc::DocumentVersion& d, const HideState& hide) {
    std::vector<VisibleEntity> out;
    const auto matcher = ruleMatcher(hide.rules);
    for (const auto& e : d.entities) {
        const std::string& id = e->typed.id;
        if (hide.keys.count(entityKey(id)) || matcher(e->typed.classname)) continue;
        VisibleEntity v;
        v.entityId = id;
        for (const auto& b : e->brushes)
            if (!hide.keys.count(brushKey(id, b->brush.id))) v.brushIds.push_back(b->brush.id);
        if (!e->brushes.empty() && v.brushIds.empty()) continue;   // every brush hidden
        out.push_back(std::move(v));
    }
    return out;
}

std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash> hiddenObjects(const scene::SceneSnapshot& snap, const HideState& hide) {
    std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash> out;
    if (!hide.any() || !snap.doc) return out;
    const auto matcher = ruleMatcher(hide.rules);
    std::map<std::string, bool> entityGone;   // by entity id: hidden by its key or class
    for (const auto& entry : snap.objects) {
        const scene::ObjectKey& key = entry.first;
        bool gone = false;
        if (!key.instance.empty()) {
            gone = hide.instanceHidden(key.instance.substr(0, key.instance.find('/')));
        } else {
            auto it = entityGone.find(key.entityId);
            if (it == entityGone.end()) {
                const int ei = snap.doc->findEntity(key.entityId);
                const bool g = hide.keys.count(entityKey(key.entityId)) || (ei >= 0 && matcher(snap.doc->entities[static_cast<size_t>(ei)]->typed.classname));
                it = entityGone.emplace(key.entityId, g).first;
            }
            gone = it->second || hide.keys.count(brushKey(key.entityId, key.brushId));
        }
        if (gone) out.insert(key);
    }
    // markers: {"", entityId, ""} for an entity hidden as a whole (point entities have no brush objects), {instanceId, "", ""} for a hidden prefab instance
    for (const auto& e : snap.doc->entities)
        if (!e->typed.classname.empty() && e->typed.classname != "worldspawn" && (hide.keys.count(entityKey(e->typed.id)) || matcher(e->typed.classname)))
            out.insert({"", e->typed.id, ""});
    for (const auto& n : snap.doc->instances)
        if (hide.instanceHidden(n->typed.id)) out.insert({n->typed.id, "", ""});
    return out;
}

} // namespace native::tools
