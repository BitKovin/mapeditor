// app/actions/keymap_edit -- see keymap_edit.h.
#include "app/actions/keymap_edit.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace native::actions {

namespace fs = std::filesystem;

// ---- contexts ----------------------------------------------------------------------------------------------------------------------

int contextLevel(const std::string& c) {
    const auto starts = [&](const char* p) { return c.rfind(p, 0) == 0; };
    if (c == "global") return 0;
    if (starts("window:")) return 1;
    if (starts("area:")) return 2;
    if (starts("view:")) return 3;
    if (starts("mode:")) return 4;
    if (starts("tool:")) return 5;
    return 6;   // modal:*, hold:* and anything unknown
}

bool contextsOverlap(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const int la = contextLevel(a), lb = contextLevel(b);
    if (la != lb) return true;
    if (la == 3) return a != "view:ortho" && b != "view:ortho";   // perspective, fly and orbit are active together; ortho never with them
    return la == 6;
}

std::vector<std::string> knownContexts(const Registry& registry, const Keymap& keymap) {
    std::set<std::string> set = {"global", "window:main", "area:viewport"};
    for (const ActionDef& d : registry.all()) {
        for (const std::string& c : d.contexts) set.insert(c);
        if (!d.holdContext.empty()) set.insert(d.holdContext);
    }
    for (const Binding& b : keymap.effective()) set.insert(b.context);
    std::vector<std::string> out(set.begin(), set.end());
    std::stable_sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) { return contextLevel(x) < contextLevel(y); });
    return out;
}

std::string defaultContextFor(const ActionDef& def) {
    if (def.contexts.empty()) return "global";
    std::string best = def.contexts.front();
    for (const std::string& c : def.contexts)
        if (contextLevel(c) > contextLevel(best)) best = c;
    return best;
}

bool bindableChord(const Chord& chord, std::string* reason) {
    const auto fail = [&](const char* r) {
        if (reason) *reason = r;
        return false;
    };
    if (chord.steps.empty()) return fail("empty input");
    for (const InputStep& s : chord.steps) {
        if (s.mods & kMeta) return fail("the Windows key belongs to the system");
        if (s.type == InputStep::Type::Key && (s.key == "Meta" || s.key == "Win" || s.key == "Windows")) return fail("the Windows key belongs to the system");
    }
    return true;
}

// ---- conflicts ---------------------------------------------------------------------------------------------------------------------

namespace {
std::string labelOf(const Registry& r, const std::string& id) {
    const ActionDef* d = r.find(id);
    return d ? d->label : id;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::vector<Conflict> conflictsFor(const Registry& registry, const Keymap& keymap, const Binding& candidate) {
    std::vector<Conflict> out;
    for (const Binding& e : keymap.effective()) {
        if (!(e.chord == candidate.chord)) continue;
        if (e.action == candidate.action && e.params == candidate.params) continue;   // the same thing elsewhere is no clash
        if (!contextsOverlap(e.context, candidate.context)) continue;
        Conflict c;
        c.existing = e;
        c.actionLabel = labelOf(registry, e.action);
        const int le = contextLevel(e.context), lc = contextLevel(candidate.context);
        if (e.context == candidate.context || le == lc) c.kind = Conflict::Kind::Same;
        else c.kind = lc > le ? Conflict::Kind::Shadows : Conflict::Kind::Shadowed;
        out.push_back(std::move(c));
    }
    return out;
}

std::string describeConflict(const Conflict& c) {
    std::string t = "Already used by " + c.actionLabel + " in " + c.existing.context;
    if (c.kind == Conflict::Kind::Shadows) t += " (the new binding is more specific and wins where both apply)";
    else if (c.kind == Conflict::Kind::Shadowed) t += " (that binding is more specific and wins where both apply)";
    return t;
}

// ---- rows ----------------------------------------------------------------------------------------------------------------------

std::string bindingText(const Binding& b) {
    std::string t = b.input;
    if (b.params.is_object() && !b.params.empty()) {   // "Numpad 7 [top]": what the parameters say, without the key names
        std::string values;
        for (const auto& item : b.params.items()) {
            if (!values.empty()) values += ", ";
            values += item.value().is_string() ? item.value().get<std::string>() : item.value().dump();
        }
        t += " [" + values + "]";
    }
    if (b.context != "global") t += " (" + b.context + ")";
    return t;
}

std::vector<std::string> categories(const Registry& registry) {
    std::vector<std::string> out;
    for (const ActionDef& d : registry.all())
        if (std::find(out.begin(), out.end(), d.category) == out.end()) out.push_back(d.category);
    return out;
}

std::vector<ActionRow> buildRows(const Registry& registry, const Keymap& keymap, const RowFilter& filter) {
    std::set<std::string> conflicting;
    for (const auto& [x, y] : keymap.conflicts()) {
        conflicting.insert(x->action);
        conflicting.insert(y->action);
    }
    std::vector<std::string> words;
    {
        std::istringstream in(lower(filter.text));
        for (std::string w; in >> w;) words.push_back(w);
    }
    std::vector<ActionRow> rows;
    for (const ActionDef& def : registry.all()) {
        ActionRow row;
        row.def = &def;
        for (const Binding* b : keymap.bindingsFor(def.id)) row.bindings.push_back(*b);
        row.unbound = row.bindings.empty();
        row.conflicting = conflicting.count(def.id) > 0;
        const auto hidesPreset = [&](const Binding& u) {
            return std::any_of(keymap.presetBindings().begin(), keymap.presetBindings().end(),
                               [&](const Binding& p) { return p.action == def.id && p.context == u.context && p.input == u.input; });
        };
        row.changed = std::any_of(keymap.userBindings().begin(), keymap.userBindings().end(), [&](const Binding& u) { return u.action == def.id || hidesPreset(u); }) ||
                      std::any_of(keymap.userRemoved().begin(), keymap.userRemoved().end(), [&](const Binding& u) { return u.action == def.id; });

        if (filter.changed && !row.changed) continue;
        if (filter.unbound && !row.unbound) continue;
        if (filter.conflicts && !row.conflicting) continue;
        if (!filter.category.empty() && def.category != filter.category) continue;
        if (!filter.context.empty()) {
            const bool inBinding = std::any_of(row.bindings.begin(), row.bindings.end(), [&](const Binding& b) { return b.context == filter.context; });
            const bool available = def.contexts.empty() || std::find(def.contexts.begin(), def.contexts.end(), filter.context) != def.contexts.end();
            if (!inBinding && !available) continue;
        }
        if (!words.empty()) {
            std::string hay = lower(def.id + " " + def.label + " " + def.category + " " + def.description);
            for (const Binding& b : row.bindings) hay += " " + lower(b.input + " " + b.context);
            const bool all = std::all_of(words.begin(), words.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; });
            if (!all) continue;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---- reverse lookup --------------------------------------------------------------------------------------------------------------------

std::vector<Lookup> lookupInput(const Registry& registry, const Keymap& keymap, const std::string& chordText) {
    std::vector<Lookup> out;
    Chord chord;
    if (!parseChord(chordText, chord)) return out;
    for (const Binding& b : keymap.effective()) {
        if (!(b.chord == chord)) continue;
        out.push_back({b.action, labelOf(registry, b.action), b.context, b});
    }
    return out;
}

// ---- editing ---------------------------------------------------------------------------------------------------------------------------

void BindingEditor::push() {
    undo_.push_back(keymap_.userToJson());
    redo_.clear();
}

bool BindingEditor::restore(const std::string& userJson) {
    std::string err;
    if (!keymap_.loadUser(userJson, &err)) return false;
    ++revision_;
    return true;
}

bool BindingEditor::canKeepBoth(const std::vector<Conflict>& conflicts) {
    return std::all_of(conflicts.begin(), conflicts.end(), [](const Conflict& c) { return c.kind != Conflict::Kind::Same || c.existing.fromUser; });
}

BindingEditor::Result BindingEditor::add(const Binding& binding, Resolve onConflict) {
    Result r;
    if (!registry_.find(binding.action)) {
        r.error = "unknown action \"" + binding.action + "\"";
        return r;
    }
    std::string reason;
    if (!bindableChord(binding.chord, &reason)) {
        r.error = reason;
        return r;
    }
    const auto& eff = keymap_.effective();
    if (std::any_of(eff.begin(), eff.end(), [&](const Binding& e) { return e.sameAs(binding); })) {   // already there
        r.ok = true;
        return r;
    }
    r.conflicts = conflictsFor(registry_, keymap_, binding);
    if (!r.conflicts.empty() && onConflict == Resolve::Cancel) {
        r.error = describeConflict(r.conflicts.front());
        return r;
    }
    if (onConflict == Resolve::KeepBoth && !canKeepBoth(r.conflicts)) {
        r.error = "Both cannot stay: the input is used in the same context by a binding of the preset. " + describeConflict(r.conflicts.front());
        return r;
    }
    const std::string before = keymap_.userToJson();
    if (onConflict == Resolve::Replace)
        for (const Conflict& c : r.conflicts) keymap_.unbindUser(c.existing);
    keymap_.bindUser(binding);
    undo_.push_back(before);
    redo_.clear();
    ++revision_;
    r.ok = true;
    return r;
}

BindingEditor::Result BindingEditor::add(const std::string& action, const std::string& context, const std::string& input, const nlohmann::json& params,
                                         Resolve onConflict) {
    Binding b;
    Result r;
    if (!makeBinding(action, context, input, params, false, b, &r.error)) return r;
    return add(b, onConflict);
}

bool BindingEditor::remove(const Binding& binding) {
    const std::string before = keymap_.userToJson();
    if (!keymap_.unbindUser(binding)) return false;
    undo_.push_back(before);
    redo_.clear();
    ++revision_;
    return true;
}

bool BindingEditor::resetAction(const std::string& action) {
    const std::string before = keymap_.userToJson();
    if (!keymap_.resetUserAction(action)) return false;
    undo_.push_back(before);
    redo_.clear();
    ++revision_;
    return true;
}

bool BindingEditor::resetAll() {
    if (keymap_.userEmpty()) return false;
    push();
    keymap_.clearUser();
    ++revision_;
    return true;
}

bool BindingEditor::undo() {
    if (undo_.empty()) return false;
    const std::string current = keymap_.userToJson();
    const std::string target = undo_.back();
    if (!restore(target)) return false;
    undo_.pop_back();
    redo_.push_back(current);
    return true;
}

bool BindingEditor::redo() {
    if (redo_.empty()) return false;
    const std::string current = keymap_.userToJson();
    const std::string target = redo_.back();
    if (!restore(target)) return false;
    redo_.pop_back();
    undo_.push_back(current);
    return true;
}

// ---- files -------------------------------------------------------------------------------------------------------------------------

namespace {
bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
std::string writeAtomic(const fs::path& file, const std::string& text) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + file.string();
        out << text;
        if (!out) return "cannot write " + file.string();
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        std::error_code ec2;
        fs::rename(tmp, file, ec2);
        if (ec2) return "cannot write " + file.string() + ": " + ec2.message();
    }
    return {};
}
} // namespace

std::vector<std::string> KeymapStore::names() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        out.push_back(it->path().stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string KeymapStore::checkName(const std::string& name) {
    if (name.empty()) return "The name is empty.";
    if (name.size() > 60) return "The name is too long.";
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ' || name.back() == '.') return "The name cannot start or end with a dot or a space.";
    for (const char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.')) return "Use letters, digits, spaces, - _ and . only.";
    return {};
}

std::string KeymapStore::load(const std::string& name, Keymap& keymap) const {
    if (const std::string bad = checkName(name); !bad.empty()) return bad;
    std::string text;
    if (!readAll(pathOf(name), text)) {
        keymap.clearUser();
        return {};
    }
    Keymap tmp = keymap;
    std::string err;
    if (!tmp.loadUser(text, &err)) return pathOf(name).string() + ": " + err;
    keymap = std::move(tmp);
    return {};
}

std::string KeymapStore::save(const std::string& name, const Keymap& keymap) const {
    if (const std::string bad = checkName(name); !bad.empty()) return bad;
    return writeAtomic(pathOf(name), keymap.userToJson());
}

std::string KeymapStore::remove(const std::string& name) const {
    if (const std::string bad = checkName(name); !bad.empty()) return bad;
    std::error_code ec;
    fs::remove(pathOf(name), ec);
    return ec ? ec.message() : std::string();
}

std::string KeymapStore::importFile(const fs::path& file, const std::string& name) const {
    if (const std::string bad = checkName(name); !bad.empty()) return bad;
    std::string text;
    if (!readAll(file, text)) return "cannot read " + file.string();
    Keymap probe;
    std::string err;
    if (!probe.loadUser(text, &err)) return file.string() + ": " + err;
    return writeAtomic(pathOf(name), text);
}

std::string KeymapStore::exportFile(const fs::path& file, const Keymap& keymap) const { return writeAtomic(file, keymap.userToJson()); }

} // namespace native::actions
