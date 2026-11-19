// doc/selection -- see selection.h.
#include "doc/selection.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace native::doc {

namespace {

// Refs of the document itself (no instance path) that name a brush and no face: the ones that can collapse into an entity.
bool plainBrush(const SelRef& s) { return s.instancePath.empty() && !s.brushId.empty() && !s.faceId; }

const EntityNode* entityOf(const DocumentVersion& doc, const std::string& id) {
    const int i = doc.findEntity(id);
    return i < 0 ? nullptr : doc.entities[static_cast<size_t>(i)].get();
}

bool isWorld(const EntityNode& e) { return e.typed.classname == "worldspawn"; }

const BrushNode* brushOf(const EntityNode& e, const std::string& brushId) {
    for (const auto& b : e.brushes)
        if (b->brush.id == brushId) return b.get();
    return nullptr;
}

} // namespace

SelRef wholeEntityIfSole(const DocumentVersion& doc, const SelRef& sel) {
    if (!plainBrush(sel)) return sel;
    const EntityNode* e = entityOf(doc, sel.entityId);
    if (e && !isWorld(*e) && e->brushes.size() == 1 && e->brushes[0]->brush.id == sel.brushId) return SelRef::entity(sel.entityId);
    return sel;
}

std::vector<SelRef> wholeEntitiesIfAll(const DocumentVersion& doc, const std::vector<SelRef>& list) {
    std::unordered_map<std::string, std::unordered_set<std::string>> picked;
    for (const SelRef& s : list)
        if (plainBrush(s)) picked[s.entityId].insert(s.brushId);
    std::unordered_set<std::string> whole;
    for (const auto& [id, brushes] : picked) {
        const EntityNode* e = entityOf(doc, id);
        if (!e || isWorld(*e) || e->brushes.size() < 2) continue;
        const bool all = std::all_of(e->brushes.begin(), e->brushes.end(), [&](const auto& b) { return brushes.count(b->brush.id) != 0; });
        if (all) whole.insert(id);
    }
    if (whole.empty()) return list;
    std::unordered_map<std::string, size_t> last;
    for (size_t i = 0; i < list.size(); ++i)
        if (plainBrush(list[i]) && whole.count(list[i].entityId)) last[list[i].entityId] = i;
    std::vector<SelRef> out;
    for (size_t i = 0; i < list.size(); ++i) {
        const SelRef& s = list[i];
        if (!plainBrush(s) || !whole.count(s.entityId)) {
            out.push_back(s);
        } else if (last[s.entityId] == i) {
            const bool already = std::any_of(out.begin(), out.end(), [&](const SelRef& o) { return o.instancePath.empty() && o.entityId == s.entityId && o.brushId.empty(); });
            if (!already) out.push_back(SelRef::entity(s.entityId));
        }
    }
    return out;
}

SelRef soleBrushOf(const DocumentVersion& doc, const SelRef& sel) {
    if (!sel.instancePath.empty() || !sel.brushId.empty()) return sel;
    const EntityNode* e = entityOf(doc, sel.entityId);
    if (e && !isWorld(*e) && e->brushes.size() == 1) return SelRef::brush(sel.entityId, e->brushes[0]->brush.id);
    return sel;
}

std::optional<SelRef> followInto(const DocumentVersion& doc, const SelRef& sel) {
    if (!sel.instancePath.empty()) {
        // An instance of the document, or something inside one: the instance has to exist (what is inside is the expansion's business, T2.6).
        const std::string& id = sel.instancePath.front();
        const bool exists = std::any_of(doc.instances.begin(), doc.instances.end(), [&](const auto& n) { return n->typed.id == id; });
        return exists ? std::optional<SelRef>(sel) : std::nullopt;
    }
    const EntityNode* e = entityOf(doc, sel.entityId);
    if (sel.brushId.empty()) return e ? std::optional<SelRef>(sel) : std::nullopt;
    const BrushNode* brush = e ? brushOf(*e, sel.brushId) : nullptr;
    if (!brush) {
        // The brush may have moved to another entity (a concave brush became a func_detail): look for it by id.
        e = nullptr;
        for (const auto& en : doc.entities)
            if ((brush = brushOf(*en, sel.brushId))) {
                e = en.get();
                break;
            }
        if (!e) return std::nullopt;
    }
    if (sel.faceId) {
        const bool has = std::any_of(brush->brush.faces.begin(), brush->brush.faces.end(), [&](const core::BrushFace& f) { return f.id == *sel.faceId; });
        if (!has) return std::nullopt;
    }
    if (e->typed.id == sel.entityId) return sel;
    SelRef moved = sel;
    moved.entityId = e->typed.id;
    return moved;
}

// ---- Selection ---------------------------------------------------------------------------------------------------------------------

void Selection::changed() {
    ++revision_;
    if (listener_) listener_();
}

bool Selection::contains(const SelRef& r) const { return std::find(items_.begin(), items_.end(), r) != items_.end(); }

void Selection::setItems(std::vector<SelRef> items) { items_ = std::move(items); }

void Selection::select(const std::optional<SelRef>& ref) {
    const bool sameBrush = ref && !items_.empty() && ref->entityId == items_.back().entityId && ref->brushId == items_.back().brushId &&
                           ref->instancePath == items_.back().instancePath;
    std::vector<SelRef> items;
    if (ref) items.push_back(*ref);
    const bool dropMesh = !sameBrush && !mesh_.empty();
    if (items == items_ && !dropMesh) return;
    items_ = std::move(items);
    if (!sameBrush) mesh_ = {};
    changed();
}

void Selection::toggle(const DocumentVersion& doc, const SelRef& ref) {
    std::vector<SelRef> next = items_;
    auto it = std::find(next.begin(), next.end(), ref);
    if (it != next.end()) {
        next.erase(it);
    } else {
        next.push_back(ref);
        next = wholeEntitiesIfAll(doc, next);
    }
    if (next == items_) return;
    items_ = std::move(next);
    changed();
}

void Selection::selectMany(const DocumentVersion& doc, const std::vector<SelRef>& list) {
    std::vector<SelRef> next = wholeEntitiesIfAll(doc, list);
    if (next == items_ && mesh_.empty()) return;
    items_ = std::move(next);
    mesh_ = {};
    changed();
}

Selection::ModeResult Selection::setMode(const DocumentVersion& doc, EditMode mode) {
    const SelRef* picked = primary();
    if (mode == EditMode::Mesh) {
        std::optional<SelRef> sel;
        if (picked) sel = soleBrushOf(doc, *picked);   // an entity of one brush is edited through that brush
        if (!sel || sel->brushId.empty()) return {false, "Select a brush first (click it), then press Tab to edit its mesh"};
        mode_ = mode;
        items_ = {*sel};
        mesh_ = {};
        changed();
        return {};
    }
    // Leaving face mode drops a face selection down to its brush; leaving mesh mode puts a sole brush back to its entity.
    std::vector<SelRef> items = items_;
    if (mode == EditMode::Object && picked && picked->faceId) {
        SelRef down = *picked;
        down.faceId.reset();
        items = {down};
    }
    if (mode == EditMode::Object && mode_ == EditMode::Mesh && !items.empty()) items.back() = wholeEntityIfSole(doc, items.back());
    if (mode == mode_ && items == items_ && mesh_.empty()) return {};
    mode_ = mode;
    mesh_ = {};
    if (items != items_) items_ = std::move(items);
    changed();
    return {};
}

void Selection::setMeshElement(MeshElement e) {
    if (e == meshElement_ && mesh_.empty()) return;
    meshElement_ = e;
    mesh_ = {};
    changed();
}

void Selection::setMesh(MeshSel sel) {
    if (sel == mesh_) return;
    mesh_ = std::move(sel);
    changed();
}

void Selection::reset() {
    if (items_.empty() && mode_ == EditMode::Object && mesh_.empty()) return;
    items_.clear();
    mode_ = EditMode::Object;
    mesh_ = {};
    changed();
}

void Selection::follow(const DocumentVersion& doc, bool dropMesh) {
    std::vector<SelRef> next;
    next.reserve(items_.size());
    for (const SelRef& s : items_)
        if (auto f = followInto(doc, s)) next.push_back(std::move(*f));
    EditMode mode = mode_;
    // Mesh mode needs its brush: when the primary selection lost it, back to object mode.
    if (mode == EditMode::Mesh && (next.empty() || next.back().brushId.empty())) mode = EditMode::Object;
    const bool mesh = dropMesh && !mesh_.empty();
    if (next == items_ && mode == mode_ && !mesh && mode == mode_) return;
    items_ = std::move(next);
    mode_ = mode;
    if (mesh || mode != EditMode::Mesh) mesh_ = {};
    changed();
}

} // namespace native::doc
