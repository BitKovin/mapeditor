// tools/details -- see details.h.
#include "tools/details.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/entity_class.h"
#include "tools/properties.h"

namespace native::tools {

namespace {

using Kind = DetailsEdit::Kind;

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string kv(const core::Entity& e, const std::string& key) {
    const auto it = e.keyvalues.find(key);
    return it == e.keyvalues.end() ? std::string() : it->second;
}

bool isNumber(const std::string& v) {
    if (trimmed(v).empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end != v.c_str() && *trimmed(std::string(end)).c_str() == '\0' && std::isfinite(d);
}

size_t wordCount(const std::string& v) {
    size_t n = 0;
    bool in = false;
    for (const char c : v) {
        const bool sep = std::isspace(static_cast<unsigned char>(c)) || c == ',';
        if (!sep && !in) ++n;
        in = !sep;
    }
    return n;
}

DetailsRow section(std::string text) {
    DetailsRow r;
    r.kind = RowKind::Section;
    r.label = std::move(text);
    return r;
}

DetailsRow note(std::string text) {
    DetailsRow r;
    r.kind = RowKind::Note;
    r.label = std::move(text);
    return r;
}

DetailsRow textRow(std::string label, std::string value, DetailsEdit edit, bool ro) {
    DetailsRow r;
    r.kind = RowKind::Text;
    r.label = std::move(label);
    r.value = std::move(value);
    r.edit = std::move(edit);
    r.readOnly = ro;
    return r;
}

DetailsRow staticRow(std::string label, std::string value) {
    DetailsRow r;
    r.kind = RowKind::Static;
    r.label = std::move(label);
    r.value = std::move(value);
    r.readOnly = true;
    return r;
}

DetailsRow vecRow(std::string label, const std::array<double, 3>& v, double step, DetailsEdit edit, bool ro) {
    DetailsRow r;
    r.kind = RowKind::Vec3;
    r.label = std::move(label);
    for (size_t i = 0; i < 3; ++i) r.vec[i] = formatNumber(v[i]);
    r.step = step;
    r.edit = std::move(edit);
    r.readOnly = ro;
    return r;
}

DetailsRow checkRow(std::string label, std::string tip, bool checked, DetailsEdit edit, bool ro) {
    DetailsRow r;
    r.kind = RowKind::Check;
    r.label = std::move(label);
    r.tip = std::move(tip);
    r.checked = checked;
    r.edit = std::move(edit);
    r.readOnly = ro;
    return r;
}

std::array<double, 3> arr(const core::Vec3& v) { return {v.x, v.y, v.z}; }

// The typed row of one key of the .ent class.
DetailsRow typedRow(const core::EntKey& k, const core::Entity& e, bool ro) {
    DetailsRow r;
    r.label = k.name.empty() ? k.key : k.name;
    r.tip = (k.key != k.name ? k.key + " (" + k.type + ")" : k.type) + (k.description.empty() ? "" : "\n" + k.description);
    r.readOnly = ro;
    const bool isSet = e.keyvalues.count(k.key) > 0;
    const std::string value = currentValue(e, k);
    r.value = value;
    r.changed = !isDefault(e, k);
    r.resettable = !ro && r.changed;
    r.min = k.min;
    r.max = k.max;
    r.step = k.step;
    r.edit.kind = Kind::Keyvalue;
    r.edit.entityId = e.id;
    r.edit.key = k.key;
    r.edit.emptyRemoves = !k.defaultValue;
    r.reset.kind = Kind::Keyvalue;
    r.reset.entityId = e.id;
    r.reset.key = k.key;
    r.reset.remove = true;
    r.resetLabel = k.defaultValue ? "Back to the default (" + *k.defaultValue + ")" : "Remove the key";
    switch (fieldKind(k)) {
        case FieldKind::Boolean:
            r.kind = RowKind::Check;
            r.checked = isChecked(value);
            r.edit.kind = Kind::KeyvalueBool;
            return r;
        case FieldKind::Asset:
            r.kind = RowKind::Asset;
            return r;
        case FieldKind::Color01:
        case FieldKind::Color255: {
            const bool c255 = fieldKind(k) == FieldKind::Color255;
            r.kind = RowKind::Color;
            auto raw = parseColor1(value);
            std::array<double, 3> c = raw ? *raw : std::array<double, 3>{1, 1, 1};
            if (raw && c255)
                for (double& x : c) x /= 255;
            r.vec = {colorToHex(c), "", ""};
            r.edit.kind = Kind::KeyvalueColor;
            r.edit.color255 = c255;
            return r;
        }
        case FieldKind::List: {
            r.kind = RowKind::Choice;
            bool listed = false;
            for (const auto& [name, v] : k.items) {
                r.choices.push_back({name, v});
                if (v == value) listed = true;
            }
            if (!listed) r.choices.insert(r.choices.begin(), {isSet ? value : std::string("\xE2\x80\x94"), value});   // keep a value the definition does not list
            r.edit.emptyRemoves = !isSet;
            return r;
        }
        case FieldKind::Integer:
        case FieldKind::Real:
            if (value.empty() || isNumber(value)) {
                r.kind = fieldKind(k) == FieldKind::Integer ? RowKind::Integer : RowKind::Real;
                r.edit.kind = Kind::KeyvalueNumber;
                if (!r.step) r.step = r.kind == RowKind::Integer ? 1.0 : 0.0;   // 0 = any
                return r;
            }
            break;
        case FieldKind::Vec3:
            if (const auto parts = parseVec3(value);
                trimmed(value).empty() || (wordCount(value) == 3 && std::all_of(parts.begin(), parts.end(), [](const auto& x) { return x.has_value(); }))) {
                r.kind = RowKind::Vec3;
                for (size_t i = 0; i < 3; ++i) r.vec[i] = parts[i] ? formatNumber(*parts[i]) : std::string();
                r.edit.kind = Kind::KeyvalueVec;
                if (!r.step) r.step = 0.0;
                return r;
            }
            break;
        default: break;
    }
    r.kind = RowKind::Text;
    return r;
}

// A reference row: one picker per target plus one to add another.
DetailsRow refRow(const std::string& label, const std::string& key, const std::string& tip, std::vector<std::string> paths, const std::string& exclude, bool ro,
                  DetailsEdit edit, const std::vector<RefTarget>& targets) {
    DetailsRow r;
    r.kind = RowKind::Ref;
    r.label = label;
    r.tip = (label != key ? key + "\n" : std::string()) + tip;
    r.paths = std::move(paths);
    r.excludePath = exclude;
    r.readOnly = ro;
    r.edit = std::move(edit);
    r.edit.key = key;
    // one key holds one name: targets with different names cannot all be reached (the compiler writes the first one's name)
    std::vector<std::string> names;
    for (const std::string& p : r.paths)
        for (const RefTarget& t : targets)
            if (t.path == p && !t.resolvedName.empty() && std::find(names.begin(), names.end(), t.resolvedName) == names.end()) names.push_back(t.resolvedName);
    if (r.paths.size() > 1 && names.size() > 1) {
        std::string list;
        for (size_t i = 0; i < names.size(); ++i) list += (i ? ", " : "") + names[i];
        r.warning = "The targets have different names (" + list + "): only the first is written.";
        r.canShare = !ro && std::none_of(r.paths.begin(), r.paths.end(), [](const std::string& p) { return p.find('/') != std::string::npos; });
    }
    return r;
}

const core::EntClass* findDef(const core::EntityDefinitions* defs, const std::string& cls) { return defs ? defs->find(cls) : nullptr; }

std::string describe(const core::EntClass& c) {
    std::string d = c.description;
    return d.size() > 400 ? d.substr(0, 400) + "\xE2\x80\xA6" : d;
}

void entityRows(DetailsForm& f, const DetailsContext& ctx, const doc::EntityNode& node, const std::string& brushIdIn) {
    const core::Entity& e = node.typed;
    const bool ro = ctx.readOnly;
    const core::EntClass* def = findDef(ctx.defs, e.classname);
    const bool brushEntity = isBrushEntityNode(node);
    const bool world = e.classname == "worldspawn";
    std::string brushId = brushIdIn;
    if (brushId.empty() && brushEntity && !world && node.brushes.size() == 1) brushId = node.brushes[0]->brush.id;
    f.entityId = e.id;
    f.title = e.classname;
    f.targets = refTargets(*ctx.doc, ctx.prefab ? *ctx.prefab : std::vector<core::ResolvedEntity>{}, ctx.library);
    if (ctx.defs)
        for (const core::EntClass& c : ctx.defs->classes) {
            std::string d = c.description.substr(0, c.description.find('\n'));
            if (d.size() > 60) d.resize(60);
            f.classes.push_back({c.name, std::string(c.kind == core::EntClass::Kind::Group ? "brush" : "point") + (d.empty() ? "" : " - " + d)});
        }

    DetailsEdit base;
    base.entityId = e.id;
    DetailsEdit ed = base;
    ed.kind = Kind::Classname;
    f.rows.push_back(textRow("classname", e.classname, ed, ro));
    f.rows.push_back(staticRow("id", e.id));
    ed = base;
    ed.kind = Kind::Keyvalue;
    ed.key = "targetname";
    ed.emptyRemoves = true;
    {
        DetailsRow tn = textRow("targetname", kv(e, "targetname"), ed, ro);
        tn.tip = "targetname: the name other entities use to target this one";
        f.rows.push_back(std::move(tn));
    }
    if (def && !def->description.empty()) f.rows.push_back(note(describe(*def)));

    const bool customOrigin = brushEntity && core::EntityClassRegistry::showsCustomOriginGizmo(e.classname);
    if (!brushEntity || customOrigin) {
        ed = base;
        ed.kind = Kind::Origin;
        f.rows.push_back(vecRow(customOrigin ? "origin (pivot)" : "origin", arr(e.origin), 0.05, ed, ro));
    }

    if (!brushId.empty()) {
        const int bi = ctx.doc->findBrush(e.id, brushId);
        if (bi >= 0) {
            const core::Brush& b = node.brushes[static_cast<size_t>(bi)]->brush;
            f.rows.push_back(section("Brush " + b.id));
            DetailsRow op;
            op.kind = RowKind::Choice;
            op.label = "operation";
            op.choices = {{"Additive", "add"}, {"Subtractive", "subtract"}};
            op.value = b.op == core::BrushOp::Subtract ? "subtract" : "add";
            op.readOnly = ro;
            op.edit.kind = Kind::BrushOp;
            op.edit.entityId = e.id;
            op.edit.brushId = b.id;
            f.rows.push_back(std::move(op));
            if (b.op == core::BrushOp::Subtract) {
                f.rows.push_back(note("Carves brushes of:"));
                const std::pair<const char*, bool> boxes[] = {{"its own entity", b.carve.self}, {"structural entities", b.carve.structural}, {"other non-structural entities", b.carve.nonStructural}};
                for (int i = 0; i < 3; ++i) {
                    DetailsEdit c;
                    c.kind = Kind::Carve;
                    c.entityId = e.id;
                    c.brushId = b.id;
                    c.axis = i;
                    f.rows.push_back(checkRow(boxes[i].first, "", boxes[i].second, c, ro));
                }
            }
            f.rows.push_back(note(std::to_string(b.vertices.size()) + " vertices \xC2\xB7 " + std::to_string(b.faces.size()) + " faces"));
        }
    }

    if (brushEntity && !world) {   // `_vertex_lit` (Core, bsp_compile): the entity keeps no lightmap texels in the export; its light is only in the vertex colour
        ed = base;
        ed.kind = Kind::VertexLit;
        const std::string v = kv(e, "_vertex_lit");
        f.rows.push_back(checkRow("Vertex lit only",
                                  "Export without lightmaps: the light is read at the vertices and stored in the vertex colour (smaller files, coarser light; only light style 0). Good for small detail geometry.",
                                  e.keyvalues.count("_vertex_lit") && !v.empty() && v != "0" && v != "false", ed, ro));
    }

    if (def && !def->flags.empty()) {
        f.rows.push_back(section("Spawnflags"));
        int current = 0;
        try { current = std::stoi(kv(e, "spawnflags")); } catch (...) { current = 0; }
        std::vector<core::EntFlag> flags = def->flags;
        std::sort(flags.begin(), flags.end(), [](const auto& a, const auto& b) { return a.bit < b.bit; });
        for (const core::EntFlag& fl : flags) {
            ed = base;
            ed.kind = Kind::Spawnflag;
            ed.bit = fl.bit;
            f.rows.push_back(checkRow(fl.name, fl.description.empty() ? fl.name : fl.description, (current & (1 << fl.bit)) != 0, ed, ro));
        }
    }

    ed = base;
    ed.kind = Kind::Angle;
    f.rows.push_back(vecRow("angles (\xC2\xB0)", arr(e.angles), 5, ed, ro));
    if (!brushEntity && e.classname != "decal") {
        ed = base;
        ed.kind = Kind::Scale;
        f.rows.push_back(vecRow("scale", parseScaleVec(kv(e, "modelscale_vec")), 0.05, ed, ro));
    }

    // every key the class defines, in .ent order, with a widget chosen by its type; a target key is the entity picker.
    // Keys the form edits elsewhere are left out: targetname (top), angle / angles (the angles row), modelscale_vec (the scale row), spawnflags (the checkboxes)
    std::vector<std::string> hidden = {"classname", "targetname", "origin", "angle", "angles"};
    if (brushEntity) hidden.push_back("_vertex_lit");
    if (!brushEntity && e.classname != "decal") hidden.push_back("modelscale_vec");
    if (def && !def->flags.empty()) hidden.push_back("spawnflags");
    const std::vector<const core::EntKey*> typed = propertyKeys(def, hidden);
    const auto isTyped = [&](const std::string& k) { return std::any_of(typed.begin(), typed.end(), [&](const core::EntKey* t) { return t->key == k; }); };
    if (!typed.empty()) {
        f.rows.push_back(section(def->name.rfind("light_", 0) == 0 ? "Light" : "Properties"));
        for (const core::EntKey* k : typed) {
            if (fieldKind(*k) == FieldKind::Ref) {
                DetailsEdit r = base;
                r.kind = Kind::Ref;
                const auto it = e.refs.find(k->key);
                f.rows.push_back(refRow(k->name.empty() ? k->key : k->name, k->key, k->description, it == e.refs.end() ? std::vector<std::string>{} : it->second.paths, e.id, ro, r, f.targets));
            } else {
                f.rows.push_back(typedRow(*k, e, ro));
            }
        }
    }
    const auto shown = [&](const std::string& k) { return std::find(hidden.begin(), hidden.end(), k) != hidden.end() || isTyped(k); };
    // keys the class does not define: references set on the entity (or just added) keep their picker, the rest are text
    std::vector<std::string> otherRefs;
    const auto addRef = [&](const std::string& k) {
        if (!shown(k) && std::find(otherRefs.begin(), otherRefs.end(), k) == otherRefs.end()) otherRefs.push_back(k);
    };
    {
        std::vector<std::string> own;
        for (const auto& [k, v] : e.refs) own.push_back(k);
        std::sort(own.begin(), own.end());
        for (const std::string& k : own) addRef(k);
        for (const std::string& k : ctx.pendingRefs) addRef(k);
    }
    f.rows.push_back(section(typed.empty() ? "Keyvalues" : "Other keyvalues"));
    for (const std::string& k : otherRefs) {
        DetailsEdit r = base;
        r.kind = Kind::Ref;
        const auto it = e.refs.find(k);
        f.rows.push_back(refRow(k, k, "", it == e.refs.end() ? std::vector<std::string>{} : it->second.paths, e.id, ro, r, f.targets));
    }
    std::vector<std::string> keys;
    for (const auto& [k, v] : e.keyvalues)
        if (!shown(k)) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const std::string& k : keys) {
        DetailsRow r;
        r.kind = RowKind::KeyValue;
        r.label = k;
        r.value = e.keyvalues.at(k);
        r.readOnly = ro;
        r.edit.kind = Kind::Keyvalue;
        r.edit.entityId = e.id;
        r.edit.key = k;
        r.reset = r.edit;
        r.reset.remove = true;
        r.resettable = !ro;
        f.rows.push_back(std::move(r));
    }
    f.refKeys = refKeysOf(def);
    f.refKeys.erase(std::remove_if(f.refKeys.begin(), f.refKeys.end(), [&](const std::string& k) { return shown(k) || std::find(otherRefs.begin(), otherRefs.end(), k) != otherRefs.end(); }), f.refKeys.end());
    DetailsRow add;
    add.kind = RowKind::AddKey;
    add.readOnly = ro;
    add.edit.kind = Kind::Keyvalue;
    add.edit.entityId = e.id;
    f.rows.push_back(std::move(add));
}

void instanceRows(DetailsForm& f, const DetailsContext& ctx, const doc::InstanceNode& node) {
    const core::PrefabInstance& inst = node.typed;
    const bool ro = ctx.readOnly;
    f.instanceId = inst.id;
    f.title = "Prefab";
    const std::vector<std::string> names = instanceDisplayNames(*ctx.doc);
    std::string display;
    for (size_t i = 0; i < ctx.doc->instances.size(); ++i)
        if (ctx.doc->instances[i]->typed.id == inst.id) display = names[i];
    DetailsEdit e;
    e.instanceId = inst.id;
    e.kind = Kind::InstanceName;
    f.rows.push_back(textRow("name", display, e, ro));
    f.rows.push_back(staticRow("file", inst.file));
    f.rows.push_back(staticRow("id", inst.id));
    const std::array<double, 3> one = {1, 1, 1};
    const std::array<std::pair<const char*, double>, 3> steps = {{{"origin", 0.05}, {"angles (\xC2\xB0)", 5}, {"scale", 0.05}}};
    for (int field = 0; field < 3; ++field) {
        DetailsEdit v = e;
        v.kind = Kind::InstanceVec;
        v.field = field;
        const std::array<double, 3> cur = field == 0 ? arr(inst.origin) : field == 1 ? arr(inst.angles) : (inst.scale ? arr(*inst.scale) : one);
        f.rows.push_back(vecRow(steps[static_cast<size_t>(field)].first, cur, steps[static_cast<size_t>(field)].second, v, ro));
    }

    // contents: every entity of the prefab (children included); click one to override its values for THIS instance
    std::vector<const core::ResolvedEntity*> mine;
    if (ctx.prefab)
        for (const core::ResolvedEntity& r : *ctx.prefab)
            if (r.instancePath.size() > 1 && r.instancePath[1] == inst.id) mine.push_back(&r);
    f.targets = refTargets(*ctx.doc, ctx.prefab ? *ctx.prefab : std::vector<core::ResolvedEntity>{}, ctx.library);
    f.rows.push_back(section("Contents (" + std::to_string(mine.size()) + ")"));
    if (mine.empty()) f.rows.push_back(note("The prefab file has no entities (or is not loaded)."));
    for (const core::ResolvedEntity* r : mine) {
        std::string path;
        for (size_t i = 2; i < r->instancePath.size(); ++i) path += r->instancePath[i] + "/";
        path += r->sourceEntityId;
        const auto ov = inst.overrides.find(path);
        const std::string tn = kv(r->entity, "targetname");
        DetailsRow row;
        row.kind = RowKind::Content;
        row.label = !tn.empty() ? tn : !r->entity.classname.empty() ? r->entity.classname : r->sourceEntityId;
        row.value = path + (ov != inst.overrides.end() ? " \xE2\x97\x8F" : "");
        row.path = path;
        row.open = ctx.openPath == path;
        f.rows.push_back(row);
        if (!row.open) continue;
        // the override editor of this entity
        const core::EntityOverride* o = ov == inst.overrides.end() ? nullptr : &ov->second;
        std::vector<std::string> keys;
        for (const auto& [k, v] : r->entity.keyvalues) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const std::string& k : keys) {
            DetailsRow kr;
            kr.kind = RowKind::KeyValue;
            kr.label = k;
            kr.value = r->entity.keyvalues.at(k);
            kr.readOnly = ro;
            kr.changed = o && o->keyvalues.count(k);
            kr.edit.kind = Kind::OverrideKeyvalue;
            kr.edit.instanceId = inst.id;
            kr.edit.path = path;
            kr.edit.key = k;
            kr.reset = kr.edit;
            kr.reset.remove = true;
            kr.resettable = !ro && kr.changed;
            kr.resetLabel = "Revert to the prefab value";
            f.rows.push_back(std::move(kr));
        }
        std::vector<std::string> present;
        for (const auto& [k, v] : r->entity.refs) present.push_back(k);
        if (o)
            for (const auto& [k, v] : o->refs) present.push_back(k);
        std::sort(present.begin(), present.end());
        for (const std::string& k : refKeysOf(findDef(ctx.defs, r->entity.classname), present)) {
            const core::RefTargets* over = nullptr;
            if (o)
                if (const auto it = o->refs.find(k); it != o->refs.end()) over = &it->second;
            const bool overridden = over != nullptr;
            std::vector<std::string> paths;
            if (overridden) {
                paths = over->paths;
            } else if (const auto fr = r->entity.refs.find(k); fr != r->entity.refs.end()) {
                // the prefab file's own ref is relative to the entity's own scope: shown in root terms so it matches the list
                const auto sc = r->refScopes.find(k);
                const size_t scope = sc == r->refScopes.end() ? r->instancePath.size() - 1 : sc->second;
                std::string prefix;
                for (size_t i = 1; i < r->instancePath.size() && i <= scope; ++i) prefix += r->instancePath[i] + "/";
                for (const std::string& p : fr->second.paths) paths.push_back(prefix + p);
            }
            DetailsEdit re;
            re.kind = Kind::OverrideRef;
            re.instanceId = inst.id;
            re.path = path;
            DetailsRow rr = refRow(k, k, overridden ? "Overridden in this instance" : "From the prefab file", paths, "", ro, re, f.targets);
            rr.changed = overridden;
            f.rows.push_back(std::move(rr));
        }
        DetailsRow add;
        add.kind = RowKind::AddKey;
        add.readOnly = ro;
        add.edit.kind = Kind::OverrideKeyvalue;
        add.edit.instanceId = inst.id;
        add.edit.path = path;
        f.rows.push_back(std::move(add));
        DetailsRow del;
        del.kind = RowKind::Summary;   // the "Delete in instance" button
        del.label = "Delete in instance";
        del.tip = "Remove this entity from this instance only";
        del.path = path;
        del.readOnly = ro;
        del.reset.kind = Kind::OverrideDelete;
        del.reset.instanceId = inst.id;
        del.reset.path = path;
        del.resetLabel = "Delete in instance";
        f.rows.push_back(std::move(del));
    }

    // the overrides summary (also lists deleted entities, which no longer appear in Contents)
    if (!inst.overrides.empty()) {
        f.rows.push_back(section("Overrides (" + std::to_string(inst.overrides.size()) + ")"));
        for (const auto& [path, o] : inst.overrides) {
            std::vector<std::string> parts;
            if (o.deleted) parts.push_back("deleted");
            std::vector<std::string> kvs;
            for (const auto& [k, v] : o.keyvalues) kvs.push_back(k);
            std::sort(kvs.begin(), kvs.end());
            parts.insert(parts.end(), kvs.begin(), kvs.end());
            std::vector<std::string> rfs;
            for (const auto& [k, v] : o.refs) rfs.push_back("ref " + k);
            std::sort(rfs.begin(), rfs.end());
            parts.insert(parts.end(), rfs.begin(), rfs.end());
            if (o.origin) parts.push_back("origin");
            if (o.angles) parts.push_back("angles");
            if (o.scale) parts.push_back("scale");
            std::string text;
            for (size_t i = 0; i < parts.size(); ++i) text += (i ? ", " : "") + parts[i];
            DetailsRow s;
            s.kind = RowKind::Summary;
            s.label = path;
            s.value = text;
            s.path = path;
            s.readOnly = ro;
            s.tip = o.deleted ? "Restore" : "Revert all overrides of this entity";
            s.resetLabel = o.deleted ? "Restore" : "Revert";
            s.reset.kind = Kind::OverrideRevert;
            s.reset.instanceId = inst.id;
            s.reset.path = path;
            f.rows.push_back(std::move(s));
        }
    }
}

std::vector<std::string> pathsOf(const EditValue& v) { return v.paths; }

} // namespace

bool isBrushEntityNode(const doc::EntityNode& n) { return !n.brushes.empty() || n.typed.classname == "worldspawn"; }

DetailsForm buildDetails(const DetailsContext& ctx) {
    DetailsForm f;
    f.message = "Nothing selected.";
    if (!ctx.doc || !ctx.selection) return f;
    const doc::SelRef& sel = *ctx.selection;
    if (!sel.instancePath.empty() && !sel.entityId.empty()) {
        f.mode = DetailsForm::Mode::Inside;
        f.message = "Part of a prefab instance: select the instance to edit its overrides.";
        return f;
    }
    if (sel.isInstance()) {
        for (const auto& n : ctx.doc->instances)
            if (n->typed.id == sel.instancePath[0]) {
                f.mode = DetailsForm::Mode::Instance;
                f.message.clear();
                instanceRows(f, ctx, *n);
                return f;
            }
        return f;
    }
    const int ei = ctx.doc->findEntity(sel.entityId);
    if (ei < 0) return f;
    f.mode = DetailsForm::Mode::Entity;
    f.message.clear();
    entityRows(f, ctx, *ctx.doc->entities[static_cast<size_t>(ei)], sel.brushId);
    return f;
}

EditResult applyDetailsEdit(const std::shared_ptr<const doc::DocumentVersion>& base, const core::EntityDefinitions* defs, const DetailsEdit& edit, const EditValue& value) {
    if (!base) return {};
    const auto entityNode = [&]() -> const doc::EntityNode* {
        const int i = base->findEntity(edit.entityId);
        return i < 0 ? nullptr : base->entities[static_cast<size_t>(i)].get();
    };
    const std::string text = value.text ? *value.text : std::string();
    switch (edit.kind) {
        case Kind::None: return {};
        case Kind::Classname: return setClassname(base, edit.entityId, trimmed(text));
        case Kind::Keyvalue: {
            std::optional<std::string> v = value.text;
            if (edit.remove) v.reset();
            else if (edit.emptyRemoves && trimmed(text).empty()) v.reset();
            else if (v && edit.key == "targetname") v = trimmed(*v);
            return setKeyvalue(base, edit.entityId, edit.key, v);
        }
        case Kind::KeyvalueBool: return setKeyvalue(base, edit.entityId, edit.key, value.checked ? "1" : "0");
        case Kind::KeyvalueNumber: {
            const doc::EntityNode* n = entityNode();
            if (!n) return {};
            const core::EntClass* def = defs ? defs->find(n->typed.classname) : nullptr;
            double v = value.number;
            if (def)
                for (const core::EntKey& k : def->keys)
                    if (k.key == edit.key) {
                        const auto c = clampValue(k, v);
                        if (!c) return {};
                        v = *c;
                    }
            if (!std::isfinite(v)) return {};
            return setKeyvalue(base, edit.entityId, edit.key, formatNumber(v));
        }
        case Kind::KeyvalueVec: {
            const doc::EntityNode* n = entityNode();
            if (!n || !std::isfinite(value.number) || edit.axis < 0 || edit.axis > 2) return {};
            const core::EntClass* def = defs ? defs->find(n->typed.classname) : nullptr;
            std::string cur = kv(n->typed, edit.key);
            if (!n->typed.keyvalues.count(edit.key) && def)
                for (const core::EntKey& k : def->keys)
                    if (k.key == edit.key) cur = k.defaultValue.value_or("");
            auto parts = parseVec3(cur);
            std::array<double, 3> out{};
            for (size_t i = 0; i < 3; ++i) out[i] = parts[i].value_or(0);
            out[static_cast<size_t>(edit.axis)] = value.number;
            return setKeyvalue(base, edit.entityId, edit.key, formatNumber(out[0]) + " " + formatNumber(out[1]) + " " + formatNumber(out[2]));
        }
        case Kind::KeyvalueColor: {
            const auto c = hexToColor(text);
            if (!c) return {};
            return setKeyvalue(base, edit.entityId, edit.key, edit.color255 ? formatColor255(*c) : formatColor(*c));
        }
        case Kind::Origin: return setOrigin(base, edit.entityId, edit.axis, value.number);
        case Kind::Angle: return setAngle(base, edit.entityId, edit.axis, value.number);
        case Kind::Scale: return setScale(base, edit.entityId, edit.axis, value.number);
        case Kind::Ref: return setRef(base, edit.entityId, edit.key, pathsOf(value));
        case Kind::ShareName: return shareTargetName(base, pathsOf(value));
        case Kind::BrushOp: return setBrushOp(base, edit.entityId, edit.brushId, value.checked);
        case Kind::Carve: return setCarve(base, edit.entityId, edit.brushId, static_cast<CarveKey>(edit.axis), value.checked);
        case Kind::VertexLit: return setKeyvalue(base, edit.entityId, "_vertex_lit", value.checked ? std::optional<std::string>("1") : std::nullopt);
        case Kind::Spawnflag: {
            const doc::EntityNode* n = entityNode();
            if (!n) return {};
            int now = 0;
            try { now = std::stoi(kv(n->typed, "spawnflags")); } catch (...) { now = 0; }
            const int next = value.checked ? now | (1 << edit.bit) : now & ~(1 << edit.bit);
            return setKeyvalue(base, edit.entityId, "spawnflags", next ? std::optional<std::string>(std::to_string(next)) : std::nullopt);
        }
        case Kind::InstanceName: return renameInstance(base, edit.instanceId, text);
        case Kind::InstanceVec: return setInstanceVec(base, edit.instanceId, static_cast<InstanceVec>(edit.field), edit.axis, value.number);
        case Kind::OverrideKeyvalue: {
            std::optional<std::string> v = value.text;
            if (edit.remove) v.reset();
            return setOverrideKeyvalue(base, edit.instanceId, edit.path, edit.key, v);
        }
        case Kind::OverrideRef: return setOverrideRef(base, edit.instanceId, edit.path, edit.key, pathsOf(value));
        case Kind::OverrideDelete:
            return setOverrideField(base, edit.instanceId, edit.path, OverrideField::Deleted, std::array<double, 3>{0, 0, 0});
        case Kind::OverrideRevert: return revertOverride(base, edit.instanceId, edit.path);
    }
    return {};
}

} // namespace native::tools
