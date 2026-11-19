// doc/selection -- what is selected, and in which edit mode.
//
// Owns: SelRef (one selected thing), MeshSel (the vertices / edges / faces picked in the primary brush while mesh editing), EditMode /
// MeshElement, and Selection, the model that panels, the renderer outline and the tools read and change. The rules are the Shell's
// (shell/src/state/store.ts): a brush that is the only brush of its entity IS the entity, selecting every brush of a brush entity selects
// the entity, worldspawn stays a bag of separate brushes, the primary selection is the last one, a new version of the document keeps the
// selection that still exists (a brush that moved to another entity is followed).
// Not part of the document: not saved, not undone. Selection changes notify panels and the renderer (outline), never the scene builder.
// Thread: UI thread only.
// Spec: 03_DOCUMENT.md §8, 08_MILESTONES.md T2.5.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"

namespace native::doc {

struct SelRef {
    // A prefab instance of the document is {instancePath = {id}, entityId = ""}; something inside an instance (T2.6 on) continues the path
    // with the ids of the nested instances and names entityId / brushId inside the innermost one. [] = the document itself.
    std::vector<std::string> instancePath;
    std::string entityId;
    std::string brushId;                     // "" = the whole entity
    std::optional<uint32_t> faceId;          // set = face mode: one face of the brush (several faces = several refs)

    static SelRef entity(std::string id) { return SelRef{{}, std::move(id), {}, std::nullopt}; }
    static SelRef brush(std::string entityId, std::string brushId) { return SelRef{{}, std::move(entityId), std::move(brushId), std::nullopt}; }
    static SelRef face(std::string entityId, std::string brushId, uint32_t face) { return SelRef{{}, std::move(entityId), std::move(brushId), face}; }
    static SelRef instance(std::string id) { return SelRef{{std::move(id)}, {}, {}, std::nullopt}; }

    bool isInstance() const { return !instancePath.empty() && entityId.empty(); }
    bool isEntity() const { return entityId.size() && brushId.empty() && !faceId; }
    bool isBrush() const { return !brushId.empty() && !faceId; }
    bool isFace() const { return faceId.has_value(); }
    bool operator==(const SelRef& o) const {
        return instancePath == o.instancePath && entityId == o.entityId && brushId == o.brushId && faceId == o.faceId;
    }
    bool operator!=(const SelRef& o) const { return !(*this == o); }
};

// What is picked inside the primary brush in mesh mode. `edges` are opaque ids of the mesh tools (the half-edge list index).
struct MeshSel {
    std::vector<uint32_t> vertices, edges, faces;
    bool empty() const { return vertices.empty() && edges.empty() && faces.empty(); }
    bool operator==(const MeshSel& o) const { return vertices == o.vertices && edges == o.edges && faces == o.faces; }
};

enum class EditMode { Object, Face, Mesh };
enum class MeshElement { Vertex, Edge, Face };

// ---- rules that need the document ---------------------------------------------------------------------------------------------------
// A brush that is the only brush of its (non-worldspawn) entity is the entity; a face selection stays on its brush.
SelRef wholeEntityIfSole(const DocumentVersion& doc, const SelRef& sel);
// Every brush of a multi-brush (non-worldspawn) entity selected = the entity: its brushes collapse into one entry placed where the entity's
// last brush stood. Worldspawn brushes and face selections are left alone.
std::vector<SelRef> wholeEntitiesIfAll(const DocumentVersion& doc, const std::vector<SelRef>& list);
// The reverse, for what needs a brush (mesh editing): an entity selected as a whole whose only brush is meant.
SelRef soleBrushOf(const DocumentVersion& doc, const SelRef& sel);
// `sel` as it stands in `doc`: a brush that moved to another entity (convex -> func_detail) is followed there; nullopt when it is gone.
std::optional<SelRef> followInto(const DocumentVersion& doc, const SelRef& sel);

class Selection {
public:
    // Called after every change (items, mode, mesh selection). One listener; the main window turns it into a Qt signal.
    using Listener = std::function<void()>;
    void setListener(Listener l) { listener_ = std::move(l); }
    // Incremented by every change: cheap "did the selection change" for the renderer and panels.
    uint64_t revision() const { return revision_; }

    // The selection in selection order; the primary one (what the properties panel shows, what a gizmo sits on) is the last.
    const std::vector<SelRef>& items() const { return items_; }
    const SelRef* primary() const { return items_.empty() ? nullptr : &items_.back(); }
    bool empty() const { return items_.empty(); }
    bool contains(const SelRef& r) const;

    EditMode mode() const { return mode_; }
    MeshElement meshElement() const { return meshElement_; }
    const MeshSel& mesh() const { return mesh_; }

    // ---- changing it (mirrors Store.select / toggleSelect / selectMany / setEditMode) ----
    // Replaces the selection by one thing (or nothing). The mesh selection is dropped unless it is the same brush again.
    void select(const std::optional<SelRef>& ref);
    // Shift / Ctrl-click: adds the thing, or removes it when it is selected already. Completing a brush entity selects the entity.
    void toggle(const DocumentVersion& doc, const SelRef& ref);
    // Replaces the selection by a list (a box select, select all); brushes that make up a whole entity collapse into the entity.
    void selectMany(const DocumentVersion& doc, const std::vector<SelRef>& list);
    void clear() { select(std::nullopt); }

    struct ModeResult {
        bool ok = true;
        std::string message;   // why a switch was refused ("Select a brush first ...")
    };
    // Object / face / mesh mode. Mesh needs a brush (an entity of one brush is edited through it); leaving face mode drops a face selection
    // down to its brush, leaving mesh mode puts a sole brush back to its entity.
    ModeResult setMode(const DocumentVersion& doc, EditMode mode);
    // Which kind of mesh element is picked. The picked elements are dropped (converting them between kinds is the mesh tools' business).
    void setMeshElement(MeshElement e);
    void setMesh(MeshSel sel);

    // A new document is shown: nothing is selected, object mode.
    void reset();
    // The document changed (an edit, undo, redo): keeps what still exists in `doc` (following moved brushes) and leaves mesh mode when its
    // brush is gone. `dropMesh` also clears the mesh selection (undo / redo: the vertex indices may mean something else now).
    void follow(const DocumentVersion& doc, bool dropMesh);

private:
    void changed();
    void setItems(std::vector<SelRef> items);

    std::vector<SelRef> items_;
    EditMode mode_ = EditMode::Object;
    MeshElement meshElement_ = MeshElement::Vertex;
    MeshSel mesh_;
    uint64_t revision_ = 0;
    Listener listener_;
};

} // namespace native::doc
