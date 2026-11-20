// tools/outliner -- see outliner.h.
#include "tools/outliner.h"

#include <algorithm>
#include <cctype>

#include "tools/edit_ops.h"

namespace native::tools {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> wordsOf(const std::string& q) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : lower(q)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

EyeState eyeState(const HideState& hide, const std::string& entityId, const std::string& classname, const std::string& brushId) {
    if (!brushId.empty()) {
        if (hide.keys.count(brushKey(entityId, brushId))) return EyeState::Self;
        if (hide.keys.count(entityKey(entityId)) || matchingRule(classname, hide.rules)) return EyeState::Parent;
        return EyeState::No;
    }
    if (hide.keys.count(entityKey(entityId))) return EyeState::Self;
    if (matchingRule(classname, hide.rules)) return EyeState::Rule;
    return EyeState::No;
}

OutlinerModel buildOutliner(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& selection, const HideState& hide, const OutlinerState& state) {
    OutlinerModel m;
    const std::vector<std::string> words = wordsOf(state.query);
    const bool filtering = !words.empty();
    const auto hit = [&](const std::string& text) {
        const std::string t = lower(text);
        return std::all_of(words.begin(), words.end(), [&](const std::string& w) { return t.find(w) != std::string::npos; });
    };
    // what is selected: a brush picked inside a collapsed entity lights the entity's row; a face selection stands for its brush
    const auto entitySelected = [&](const std::string& id, bool brushesListed) {
        for (const doc::SelRef& s : selection)
            if (s.instancePath.empty() && s.entityId == id && (s.brushId.empty() || !brushesListed)) return true;
        return false;
    };
    const auto brushSelected = [&](const std::string& id, const std::string& brush) {
        for (const doc::SelRef& s : selection)
            if (s.instancePath.empty() && s.entityId == id && s.brushId == brush) return true;
        return false;
    };

    size_t shown = 0;
    for (const auto& node : doc.entities) {
        const core::Entity& e = node->typed;
        const std::string cls = e.classname.empty() ? "point_entity" : e.classname;
        const auto tn = e.keyvalues.find("targetname");
        const std::string target = tn == e.keyvalues.end() ? std::string() : trimmed(tn->second);
        const std::string eText = cls + " " + e.id + " " + target;
        const bool eHit = !filtering || hit(eText);
        const auto brushHit = [&](const doc::BrushNode& b) { return hit(eText + " " + (b.brush.op == core::BrushOp::Subtract ? "subtract" : "add") + " brush " + b.brush.id); };
        if (filtering && !eHit && !std::any_of(node->brushes.begin(), node->brushes.end(), [&](const auto& b) { return brushHit(*b); })) continue;
        ++shown;
        const size_t nBrushes = node->brushes.size();
        // while searching, an entity listed only for a matching brush is open so the brush can be seen
        const bool open = nBrushes > 0 && (state.expanded.count(e.id) || (filtering && !eHit));
        OutlinerRow row;
        row.kind = OutlinerRow::Kind::Entity;
        row.key = entityKey(e.id);
        row.entityId = e.id;
        row.label = cls;
        row.target = target;
        row.detail = e.id + (nBrushes && !open ? " \xC2\xB7 " + std::to_string(nBrushes) + (nBrushes == 1 ? " brush" : " brushes") : "");
        row.foldable = nBrushes > 0;
        row.open = open;
        row.selected = entitySelected(e.id, open);
        row.eye = eyeState(hide, e.id, cls);
        m.rows.push_back(std::move(row));
        if (!open) continue;
        for (const auto& b : node->brushes) {
            if (filtering && !eHit && !brushHit(*b)) continue;
            OutlinerRow br;
            br.kind = OutlinerRow::Kind::Brush;
            br.key = brushKey(e.id, b->brush.id);
            br.entityId = e.id;
            br.brushId = b->brush.id;
            br.label = b->brush.op == core::BrushOp::Subtract ? "\xE2\x88\x92 brush" : "+ brush";
            br.detail = b->brush.id;
            br.selected = brushSelected(e.id, b->brush.id);
            br.eye = eyeState(hide, e.id, cls, b->brush.id);
            m.rows.push_back(std::move(br));
        }
    }

    const std::vector<std::string> names = instanceDisplayNames(doc);
    for (size_t i = 0; i < doc.instances.size(); ++i) {
        const core::PrefabInstance& p = doc.instances[i]->typed;
        if (filtering && !hit(names[i] + " " + p.file + " prefab")) continue;
        ++shown;
        OutlinerRow row;
        row.kind = OutlinerRow::Kind::Instance;
        row.key = instanceKey(p.id);
        row.instanceId = p.id;
        row.label = names[i];
        row.detail = p.file;
        for (const doc::SelRef& s : selection)
            if (s.isInstance() && s.instancePath.size() == 1 && s.instancePath[0] == p.id) row.selected = true;
        row.eye = hide.keys.count(instanceKey(p.id)) ? EyeState::Self : EyeState::No;
        m.rows.push_back(std::move(row));
    }

    if (doc.entities.empty() && doc.instances.empty()) m.message = "Empty map. Add a box from the toolbar.";
    else if (filtering && !shown) m.message = "Nothing matches \xE2\x80\x9C" + trimmed(state.query) + "\xE2\x80\x9D.";
    return m;
}

std::vector<std::string> revealKeys(const doc::SelRef& p) {
    if (p.isInstance()) return {instanceKey(p.instancePath[0])};
    if (!p.instancePath.empty()) return {instanceKey(p.instancePath[0])};   // something inside an instance: the instance's row
    std::vector<std::string> keys;
    if (!p.brushId.empty()) keys.push_back(brushKey(p.entityId, p.brushId));
    keys.push_back(entityKey(p.entityId));
    return keys;
}

std::vector<std::string> RevealTracker::update(const doc::SelRef* primary) {
    std::string key;
    if (primary) {
        key = primary->entityId + "|" + primary->brushId + "|";
        for (const std::string& i : primary->instancePath) key += i + "/";
    }
    if (key == last_) return {};
    last_ = key;
    return primary ? revealKeys(*primary) : std::vector<std::string>{};
}

} // namespace native::tools
