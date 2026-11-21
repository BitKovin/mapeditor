// tools/mesh/session -- mesh mode on the document (the document side of shell/src/state/store.ts applyMeshOp + meshTarget, and the instant tools of ui/viewport.ts runTool).
// Pure: document version + the brush being edited + the mesh selection -> the next version. Handles what the mesh operations alone do not: finding the brush, the
// concave result moving into a func_detail entity (worldspawn / func_group take convex brushes only), the smooth-edge marks, and the id conversions between the
// document's selection (edges are indices into the brush's edge list) and the mesh tools' selection (edges are vertex pairs).
// Thread: any. Spec: 08_MILESTONES.md M11 item 1 (T11.1e), REQUIREMENTS section 21 / 33 H6.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/mesh/ops.h"

namespace native::tools::mesh {

// The brush mesh mode edits: the primary selection's brush (an entity selected as a whole is edited through its sole brush). Nothing inside a prefab instance.
struct MeshTarget {
    std::string entityId, brushId, classname;
    std::shared_ptr<const doc::BrushNode> node;
    const core::Brush& brush() const { return node->brush; }
};
std::optional<MeshTarget> meshTarget(const doc::DocumentVersion& d, const doc::SelRef& primary);

// The document's mesh selection stores an edge as its index in `buildTopology(brush).edgeList`; the tools use vertex pairs.
doc::MeshSel toDocSel(const core::Brush& b, const MeshSel& s);
MeshSel fromDocSel(const core::Brush& b, const doc::MeshSel& s);
doc::MeshElement toDocElement(SelMode m);
SelMode fromDocElement(doc::MeshElement e);

// Why `brush` may not be stored in an entity of `classname` (nullopt = fine): classes that need convex solids refuse anything else; a subtractive brush may be concave.
std::optional<std::string> brushProblem(const std::string& classname, const core::Brush& b);

struct MeshApply {
    std::shared_ptr<const doc::DocumentVersion> version;   // null = refused, or nothing changed (then `error` may say why)
    doc::SelRef select;                                    // the edited brush afterwards (in the new func_detail entity when it was concave)
    MeshSel sel;                                           // the mesh selection afterwards
    std::string label, message, error;
    bool movedToDetail = false;
    bool ok() const { return version != nullptr; }
};
using MeshOpFn = std::function<OpResult(const core::Brush&, const MeshSel&)>;
// Runs `op` on the brush as one edit. A result the entity's class refuses because it is concave goes into a func_detail entity (`detailId`: the entity to use, created when it
// does not exist; empty = a fresh id) when `autoDetail`, else it is refused. `target` is the primary selection. A result that changes nothing is no edit (version null, no error)
// unless `keepUnchanged` (a drag that came back to where it began): then the version is `base` itself.
MeshApply applyMeshOp(const std::shared_ptr<const doc::DocumentVersion>& base, const doc::SelRef& target, const std::string& label, const MeshOpFn& op, const MeshSel& sel,
                      bool autoDetail = true, const std::string& detailId = {}, bool keepUnchanged = false);

// ---- the instant tools (keys and the panel run them by id) ----
// Ids: "connect" J, "merge" M, "fill" F, "dissolve" X, "delete", "flip" Alt+N, "recalc" Shift+N, "bridge", "smooth", "sharp", "subdivide" (`param` = cuts).
OpResult runMeshTool(const std::string& id, const core::Brush& b, const MeshSel& sel, SelMode mode, double param = 1, const tp::InfoFn& info = {});
// The tools that only change the selection: "selectall", "selectnone", "invert", "grow", "shrink". nullopt = not such a tool.
std::optional<MeshSel> runSelectTool(const std::string& id, const core::Brush& b, const MeshSel& sel, SelMode mode);

// Marks (`on`) or clears the smooth flag of the given vertex edges (each names the two faces that meet there); pairs whose faces no longer meet are dropped. Returns the
// brush itself (equal) when nothing changed.
core::Brush markSmooth(const core::Brush& b, const std::vector<Edge>& edges, bool on);

} // namespace native::tools::mesh
