// tools/ref_targets -- see ref_targets.h.
#include "tools/ref_targets.h"

#include <algorithm>

#include "tools/edit_ops.h"

namespace native::tools {

namespace {

std::string nameOf(const core::Entity& e) {
    const auto it = e.keyvalues.find("targetname");
    return it == e.keyvalues.end() ? std::string() : it->second;
}

std::string labelOf(const core::Entity& e, const std::string& id) {
    const std::string n = nameOf(e);
    return (!n.empty() ? n : !e.classname.empty() ? e.classname : std::string("entity")) + "  (" + id + ")";
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}

} // namespace

std::vector<std::string> chainNames(const doc::DocumentVersion& doc, doc::PrefabLibrary* library, const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    std::shared_ptr<const core::Document> inner;   // the prefab file the next id is looked up in (null at the top: the document itself)
    bool top = true, lost = false;
    for (const std::string& id : ids) {
        if (lost) {
            out.push_back(id);
            continue;
        }
        if (top) {
            const std::vector<std::string> names = instanceDisplayNames(doc);
            size_t i = 0;
            for (; i < doc.instances.size(); ++i)
                if (doc.instances[i]->typed.id == id) break;
            if (i == doc.instances.size()) {
                out.push_back(id);
                lost = true;
                continue;
            }
            out.push_back(names[i]);
            inner = library ? library->get(doc.instances[i]->typed.file)->doc : nullptr;
            top = false;
            continue;
        }
        if (!inner) {
            out.push_back(id);
            lost = true;
            continue;
        }
        const std::vector<std::string> names = core::effectiveInstanceNames(*inner);
        size_t i = 0;
        for (; i < inner->prefabInstances.size(); ++i)
            if (inner->prefabInstances[i].id == id) break;
        if (i == inner->prefabInstances.size()) {
            out.push_back(id);
            lost = true;
            continue;
        }
        out.push_back(names[i]);
        inner = library ? library->get(inner->prefabInstances[i].file)->doc : nullptr;
    }
    return out;
}

std::vector<RefTarget> refTargets(const doc::DocumentVersion& doc, const std::vector<core::ResolvedEntity>& prefab, doc::PrefabLibrary* library) {
    std::vector<RefTarget> out;
    for (const auto& e : doc.entities) out.push_back({e->typed.id, labelOf(e->typed, e->typed.id), "", nameOf(e->typed)});
    for (const core::ResolvedEntity& r : prefab) {
        const std::vector<std::string> ids(r.instancePath.begin() + (r.instancePath.empty() ? 0 : 1), r.instancePath.end());
        const std::vector<std::string> names = chainNames(doc, library, ids);
        const std::string group = join(names, "|");
        const std::string own = nameOf(r.entity);
        out.push_back({(ids.empty() ? "" : join(ids, "/") + "/") + r.sourceEntityId, labelOf(r.entity, r.sourceEntityId), group, own.empty() ? std::string() : group + "|" + own});
    }
    return out;
}

std::vector<std::string> refKeysOf(const core::EntClass* def, const std::vector<std::string>& present) {
    std::vector<std::string> keys = {"target", "killtarget"};
    const auto add = [&](const std::string& k) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    };
    if (def)
        for (const core::EntKey& k : def->keys)
            if (k.type == "target_destination" || k.type == "target") add(k.key);
    for (const std::string& k : present) add(k);
    return keys;
}

} // namespace native::tools
