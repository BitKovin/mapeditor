// app/actions/palette -- see palette.h.
#include "app/actions/palette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace native::actions {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string stripMnemonics(const std::string& s) {   // "&Top" -> "Top", "&1  map.json" -> "1  map.json"
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&' && i + 1 < s.size() && s[i + 1] != '&') continue;
        out += s[i];
    }
    return out;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

} // namespace

bool paletteRunnable(const ActionDef& def) {
    if (def.id == "view.command_palette") return false;
    return def.kind == ActionKind::Trigger || def.kind == ActionKind::Toggle;
}

std::vector<PaletteEntry> paletteEntries(const Registry& registry, const Keymap& keymap, const ActionEnv& env) {
    std::vector<PaletteEntry> out;
    const auto add = [&](const ActionDef& def, const std::string& label, const nlohmann::json& params) {
        // the params the action runs with: the variant's own, else the action's defaults
        if (!registry.isEnabled(def, env, params.is_null() ? def.params : params)) return;
        PaletteEntry e;
        e.id = def.id;
        e.label = label;
        e.category = def.category;
        e.description = def.description;
        e.params = params;
        e.keys = keymap.displayText(def.id, params);
        if (e.keys.empty() && params.is_null()) e.keys = keymap.displayText(def.id);
        out.push_back(std::move(e));
    };
    for (const ActionDef& def : registry.all()) {
        if (!paletteRunnable(def)) continue;
        if (!def.variants.empty()) {
            for (const MenuVariant& v : def.variants) add(def, def.label + ": " + stripMnemonics(v.label), v.params);
        } else if (def.dynamicVariants) {
            for (const MenuVariant& v : def.dynamicVariants(env)) add(def, def.label + ": " + stripMnemonics(v.label), v.params);
        } else {
            add(def, def.label, nullptr);
        }
    }
    return out;
}

int fuzzyScore(const std::string& query, const std::string& text) {
    if (query.empty()) return 0;
    const std::string q = lower(query), t = lower(text);
    int score = 0;
    size_t qi = 0;
    bool prevMatched = false;
    int gap = 0;
    for (size_t i = 0; i < t.size() && qi < q.size(); ++i) {
        if (t[i] == q[qi]) {
            score += 10;
            if (prevMatched) score += 30;
            if (i == 0 || !isWordChar(text[i - 1])) score += 20;
            if (qi == 0 && i == 0) score += 10;   // starts with the query
            ++qi;
            prevMatched = true;
        } else {
            prevMatched = false;
            if (qi > 0) ++gap;
        }
    }
    if (qi < q.size()) return -1;
    return std::max(1, score - std::min(gap, 30));
}

std::string recentToJson(const std::vector<Recent>& list) {
    nlohmann::json a = nlohmann::json::array();
    for (const Recent& r : list) {
        nlohmann::json o = {{"id", r.id}};
        if (!r.params.is_null()) o["params"] = r.params;
        a.push_back(std::move(o));
    }
    return a.dump();
}

std::vector<Recent> recentFromJson(const std::string& text) {
    std::vector<Recent> out;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& o : j) {
        if (!o.is_object() || !o.contains("id") || !o["id"].is_string()) continue;
        Recent r;
        r.id = o["id"].get<std::string>();
        if (o.contains("params")) r.params = o["params"];
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Recent> recentUsed(std::vector<Recent> list, const Recent& used, size_t limit) {
    list.erase(std::remove(list.begin(), list.end(), used), list.end());
    list.insert(list.begin(), used);
    if (list.size() > limit) list.resize(limit);
    return list;
}

std::vector<PaletteEntry> paletteSearch(const std::vector<PaletteEntry>& entries, const Registry& registry, const std::string& query,
                                        const std::vector<Recent>& recent) {
    const auto recentRank = [&](const PaletteEntry& e) -> int {
        for (size_t i = 0; i < recent.size(); ++i)
            if (recent[i].id == e.id && recent[i].params == e.params) return static_cast<int>(i);
        return -1;
    };
    std::vector<PaletteEntry> out;
    const std::vector<std::string> ws = words(query);

    if (ws.empty()) {
        out = entries;
        std::stable_sort(out.begin(), out.end(), [&](const PaletteEntry& a, const PaletteEntry& b) {
            const int ra = recentRank(a), rb = recentRank(b);
            if ((ra >= 0) != (rb >= 0)) return ra >= 0;
            if (ra >= 0 && rb >= 0) return ra < rb;
            if (a.category != b.category) return a.category < b.category;
            return a.label < b.label;
        });
        return out;
    }

    // a typed argument: "grid 0.25"
    {
        const std::string keyword = lower(ws[0]);
        if (ws.size() >= 2) {
            std::string arg = query.substr(query.find(ws[0]) + ws[0].size());
            arg.erase(0, arg.find_first_not_of(" \t"));
            while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();
            for (const ActionDef& def : registry.all()) {
                if (def.paletteKeyword.empty() || lower(def.paletteKeyword) != keyword || def.paletteParam.empty()) continue;
                const auto base = std::find_if(entries.begin(), entries.end(), [&](const PaletteEntry& e) { return e.id == def.id && e.params.is_null(); });
                if (base == entries.end()) continue;   // not enabled now
                nlohmann::json params = def.params.is_object() ? def.params : nlohmann::json::object();
                if (def.paletteNumeric) {
                    char* end = nullptr;
                    const double v = std::strtod(arg.c_str(), &end);
                    if (arg.empty() || end == arg.c_str() || *end != '\0' || !std::isfinite(v)) continue;
                    params[def.paletteParam] = v;
                } else {
                    params[def.paletteParam] = arg;
                }
                PaletteEntry e = *base;
                e.label = def.label + ": " + arg;
                e.params = params;
                e.score = 100000;
                out.push_back(std::move(e));
                break;
            }
        }
    }

    const size_t typed = out.size();
    for (const PaletteEntry& e : entries) {
        int total = 0;
        bool all = true;
        for (const std::string& w : ws) {
            const int byId = fuzzyScore(w, e.id);   // the id is a weaker hint than the words the user reads
            const int s = std::max({fuzzyScore(w, e.label), fuzzyScore(w, e.category + " " + e.label) - 20, byId > 0 ? byId / 2 : -1});
            if (s < 0) {
                all = false;
                break;
            }
            total += s;
        }
        if (!all) continue;
        PaletteEntry m = e;
        const int r = recentRank(e);
        m.score = total + (r >= 0 ? std::max(0, 30 - 3 * r) : 0);
        out.push_back(std::move(m));
    }
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(typed), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.label < b.label;
    });
    return out;
}

// ---- hints and key help ---------------------------------------------------------------------------------------------------------------

namespace {

struct Group {
    std::string action;
    std::vector<std::string> inputs;
};

std::vector<Group> groupsIn(const Keymap& keymap, const std::string& context) {
    std::vector<Group> groups;
    for (const Binding& b : keymap.effective()) {
        if (b.context != context) continue;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.action == b.action; });
        if (it == groups.end()) {
            groups.push_back({b.action, {}});
            it = groups.end() - 1;
        }
        if (std::find(it->inputs.begin(), it->inputs.end(), b.input) == it->inputs.end()) it->inputs.push_back(b.input);
    }
    return groups;
}

std::string joinInputs(const std::vector<std::string>& inputs) {
    std::string out;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i) out += " / ";
        out += inputs[i];
    }
    return out;
}

} // namespace

std::string hintFor(const Registry& registry, const Keymap& keymap, const std::string& context) {
    std::string out;
    for (const Group& g : groupsIn(keymap, context)) {
        const ActionDef* def = registry.find(g.action);
        if (!def) continue;
        std::vector<std::string> inputs = g.inputs;   // short: "Enter" says it for "Numpad Enter", and three keys are all that fit
        if (std::find(inputs.begin(), inputs.end(), "Enter") != inputs.end())
            inputs.erase(std::remove(inputs.begin(), inputs.end(), "Numpad Enter"), inputs.end());
        if (inputs.size() > 3) inputs.resize(3);
        if (!out.empty()) out += "  \xC2\xB7  ";
        out += joinInputs(inputs) + " " + (def->hint.empty() ? def->label : def->hint);
    }
    return out;
}

std::vector<HelpSection> keyHelpSections(const Registry& registry, const Keymap& keymap, const std::vector<std::string>& activeContexts) {
    std::vector<std::string> order;
    for (auto it = activeContexts.rbegin(); it != activeContexts.rend(); ++it)
        if (*it != "global" && std::find(order.begin(), order.end(), *it) == order.end()) order.push_back(*it);
    order.push_back("global");
    std::vector<HelpSection> out;
    for (const std::string& context : order) {
        HelpSection s;
        s.context = context;
        for (const Group& g : groupsIn(keymap, context)) {
            const ActionDef* def = registry.find(g.action);
            if (def) s.lines.push_back({joinInputs(g.inputs), def->label});
        }
        if (!s.lines.empty()) out.push_back(std::move(s));
    }
    return out;
}

} // namespace native::actions
