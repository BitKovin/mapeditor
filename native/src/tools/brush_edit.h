// tools/brush_edit -- the Radiant brush operations on the document (ports of clipSelection, hollowSelection, mergeSelection, snapSelection and selectByVolume of
// shell/src/state/actions.ts): each takes the version and the selection and returns the next version (or the new selection), refusing with a message the status bar shows.
// The caller commits a version as one undo step. Brushes inside prefab instances are never edited.
// Thread: any (pure). Spec: 08_MILESTONES.md M11 item 2 (T11.2b).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/brush_ops.h"

namespace native::tools {

struct BrushEdit {
    std::shared_ptr<const doc::DocumentVersion> version;   // null = nothing was changed
    std::vector<doc::SelRef> select;                       // what to select afterwards (empty = keep the selection as it is)
    std::string label;                                     // the undo step's name
    std::string message;                                   // what the status bar says (also when refused)
    bool ok() const { return version != nullptr; }
};

// The brushes a selection covers (picked brushes, or every brush of a selected entity), in selection order, without duplicates; prefab content is skipped.
struct SelectedBrush {
    std::string entityId;
    std::shared_ptr<const doc::BrushNode> node;
    const core::Brush& brush() const { return node->brush; }
};
std::vector<SelectedBrush> selectedBrushes(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);

// Clipper: keeps the back side of `pl`, the front, or both (the second piece becomes a new brush of the same entity). Concave brushes and brushes the plane misses are left alone.
BrushEdit clipSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const Plane& pl, Keep keep);
// Make Hollow: each selected convex brush becomes wall brushes `t` thick (the first keeps the brush's id).
BrushEdit hollowSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, double t);
// Merge: the selected brushes become one convex brush, only when that adds no space the brushes did not fill. The first brush keeps its id and entity and is selected.
BrushEdit mergeSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels);
// Snaps the vertices of the selected brushes to the grid; brushes that would break are left alone and counted in the message.
BrushEdit snapSelection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, double step);

enum class VolumeMode { Inside, Touching };
struct VolumeSelect {
    std::vector<doc::SelRef> found;
    std::string message;
};
// The other convex brushes of the document that lie completely inside / touch the selected convex brushes (taken as volumes).
VolumeSelect selectByVolume(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels, VolumeMode mode);

} // namespace native::tools
