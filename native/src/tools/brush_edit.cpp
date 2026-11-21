// tools/brush_edit -- see brush_edit.h.
#include "tools/brush_edit.h"

#include <algorithm>
#include <set>

#include "doc/edit.h"
#include "tools/box_tool.h"

namespace native::tools {

using doc::SelRef;
using Base = std::shared_ptr<const doc::DocumentVersion>;

namespace {

BrushEdit refused(const std::string& message) {
    BrushEdit r;
    r.message = message;
    return r;
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

bool sameVertices(const core::Brush& a, const core::Brush& b) {
    if (a.vertices.size() != b.vertices.size() || a.faces.size() != b.faces.size()) return false;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        if (a.vertices[i].x != b.vertices[i].x || a.vertices[i].y != b.vertices[i].y || a.vertices[i].z != b.vertices[i].z) return false;
    return true;
}

} // namespace

std::vector<SelectedBrush> selectedBrushes(const doc::DocumentVersion& d, const std::vector<SelRef>& sels) {
    std::vector<SelectedBrush> out;
    std::set<std::string> seen;
    for (const SelRef& s : sels) {
        if (s.isInstance() || !s.instancePath.empty() || s.entityId.empty()) continue;
        const int ei = d.findEntity(s.entityId);
        if (ei < 0) continue;
        const doc::EntityNode& e = *d.entities[static_cast<size_t>(ei)];
        for (const auto& node : e.brushes)
            if ((s.brushId.empty() || s.brushId == node->brush.id) && seen.insert(node->brush.id).second) out.push_back({s.entityId, node});
    }
    return out;
}

BrushEdit clipSelection(const Base& base, const std::vector<SelRef>& sels, const Plane& pl, Keep keep) {
    if (!base) return refused("no document");
    const std::vector<SelectedBrush> targets = selectedBrushes(*base, sels);
    if (targets.empty()) return refused("Clipper: select a brush first");
    size_t skipped = 0, cut = 0;
    doc::VersionEditor ed(base);
    for (const SelectedBrush& t : targets) {
        if (!isConvex(t.brush())) {
            ++skipped;
            continue;
        }
        const ClipPair r = clipBy(t.brush(), pl, keep);
        std::vector<core::Brush> parts;
        if (r.back) parts.push_back(*r.back);
        if (r.front) parts.push_back(*r.front);
        if (parts.empty()) continue;
        if (parts.size() == 1 && sameVertices(parts[0], t.brush())) continue;   // the plane misses this brush
        ++cut;
        parts[0].id = t.brush().id;
        ed.replaceBrush(t.entityId, parts[0]);
        for (size_t i = 1; i < parts.size(); ++i) {
            parts[i].id = newId("b");
            ed.addBrush(t.entityId, parts[i], t.node->raw);
        }
    }
    if (!cut) return refused(skipped ? "Clipper: concave brushes cannot be clipped" : "Clipper: the plane does not cut the selected brushes");
    BrushEdit r;
    r.label = keep == Keep::Both ? "Split brushes" : "Clip brushes";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    if (skipped) r.message = "Clipped " + std::to_string(cut) + "; " + std::to_string(skipped) + " concave brush(es) skipped";
    return r;
}

BrushEdit hollowSelection(const Base& base, const std::vector<SelRef>& sels, double t) {
    if (!base) return refused("no document");
    const std::vector<SelectedBrush> targets = selectedBrushes(*base, sels);
    if (targets.empty()) return refused("Hollow: select a brush first");
    size_t done = 0;
    doc::VersionEditor ed(base);
    for (const SelectedBrush& s : targets) {
        if (!isConvex(s.brush())) continue;
        const auto walls = hollowBrush(s.brush(), t);
        if (!walls || walls->empty()) continue;
        ++done;
        for (size_t i = 0; i < walls->size(); ++i) {
            core::Brush w = (*walls)[i];
            if (i == 0) {
                w.id = s.brush().id;
                ed.replaceBrush(s.entityId, w);
            } else {
                w.id = newId("b");
                ed.addBrush(s.entityId, w, s.node->raw);
            }
        }
    }
    if (!done) return refused("Hollow: no brush can take a wall of " + std::to_string(t) + " (too thick, or not convex)");
    BrushEdit r;
    r.label = "Make hollow";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    return r;
}

BrushEdit mergeSelection(const Base& base, const std::vector<SelRef>& sels) {
    if (!base) return refused("no document");
    const std::vector<SelectedBrush> targets = selectedBrushes(*base, sels);
    if (targets.size() < 2) return refused("Merge: select two or more brushes");
    std::vector<core::Vec3> points;
    std::vector<core::Brush> brushes;
    for (const SelectedBrush& t : targets) {
        points.insert(points.end(), t.brush().vertices.begin(), t.brush().vertices.end());
        brushes.push_back(t.brush());
    }
    std::string texture = "dev/grey";
    for (const core::BrushFace& f : targets[0].brush().faces)
        if (!f.texture.empty()) {
            texture = f.texture;
            break;
        }
    const auto hull = hullBrush(points, texture);
    const double filled = unionVolume(brushes);   // overlapping brushes count once
    if (!hull || hull->signedVolume() > filled * (1 + 1e-6)) return refused("Merge: the brushes do not form a convex shape together");
    doc::VersionEditor ed(base);
    core::Brush merged = *hull;
    merged.id = targets[0].brush().id;
    merged.op = targets[0].brush().op;
    ed.replaceBrush(targets[0].entityId, merged);
    for (size_t i = 1; i < targets.size(); ++i) ed.removeBrush(targets[i].entityId, targets[i].brush().id);
    BrushEdit r;
    r.label = "Merge brushes";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    r.select = {SelRef::brush(targets[0].entityId, merged.id)};
    return r;
}

BrushEdit snapSelection(const Base& base, const std::vector<SelRef>& sels, double step) {
    if (!base) return refused("no document");
    const std::vector<SelectedBrush> targets = selectedBrushes(*base, sels);
    if (targets.empty()) return refused("Snap: select a brush first");
    size_t snapped = 0, failed = 0;
    doc::VersionEditor ed(base);
    for (const SelectedBrush& t : targets) {
        const auto r = snapBrush(t.brush(), step);
        if (!r) {
            ++failed;
            continue;
        }
        if (sameVertices(*r, t.brush())) continue;
        ++snapped;
        ed.replaceBrush(t.entityId, *r);
    }
    if (!snapped) return refused(failed ? "Snapped 0; " + std::to_string(failed) + " brush(es) would break and were left alone" : "Already on the grid");
    BrushEdit r;
    r.label = "Snap to grid";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    r.message = failed ? "Snapped " + std::to_string(snapped) + "; " + std::to_string(failed) + " brush(es) would break and were left alone" : "Snapped " + plural(snapped, "brush", "brushes");
    return r;
}

FaceDragEdit dragFaceEdit(const Base& base, const std::string& entityId, const std::string& brushId, uint32_t faceId, double dist) {
    FaceDragEdit out;
    if (!base) {
        out.error = "no document";
        return out;
    }
    const int bi = base->findBrush(entityId, brushId);
    const int ei = base->findEntity(entityId);
    if (bi < 0 || ei < 0) {
        out.error = "the brush is gone";
        return out;
    }
    const DragResult r = dragFace(base->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush, faceId, dist);
    if (!r.ok()) {
        out.error = r.error;
        return out;
    }
    doc::VersionEditor ed(base);
    ed.replaceBrush(entityId, r.brush);
    out.version = ed.finish();
    if (!out.version) out.error = ed.error();
    return out;
}

bool selectionBounds(const doc::DocumentVersion& d, const std::vector<SelRef>& sels, V3& lo, V3& hi) {
    lo = {1e300, 1e300, 1e300};
    hi = {-1e300, -1e300, -1e300};
    bool any = false;
    for (const SelectedBrush& sb : selectedBrushes(d, sels))
        for (const core::Vec3& v : sb.brush().vertices) {
            any = true;
            lo = {std::min(lo[0], v.x), std::min(lo[1], v.y), std::min(lo[2], v.z)};
            hi = {std::max(hi[0], v.x), std::max(hi[1], v.y), std::max(hi[2], v.z)};
        }
    return any;
}

VolumeSelect selectByVolume(const doc::DocumentVersion& d, const std::vector<SelRef>& sels, VolumeMode mode) {
    VolumeSelect out;
    const std::string name = mode == VolumeMode::Inside ? "inside" : "touching";
    std::vector<SelectedBrush> volumes;
    for (const SelectedBrush& v : selectedBrushes(d, sels))
        if (isConvex(v.brush())) volumes.push_back(v);
    if (volumes.empty()) {
        out.message = "Select " + name + ": select a convex brush first";
        return out;
    }
    std::set<std::string> taken;
    for (const SelectedBrush& v : volumes) taken.insert(v.brush().id);
    for (const auto& e : d.entities)
        for (const auto& node : e->brushes) {
            const core::Brush& b = node->brush;
            if (taken.count(b.id) || !isConvex(b)) continue;
            for (const SelectedBrush& v : volumes)
                if (mode == VolumeMode::Inside ? brushInside(v.brush(), b) : brushesTouch(v.brush(), b)) {
                    out.found.push_back(SelRef::brush(e->typed.id, b.id));
                    break;
                }
        }
    out.message = out.found.empty() ? "Select " + name + ": no other brush is " + name : "Selected " + plural(out.found.size(), "brush", "brushes") + " " + name;
    return out;
}

} // namespace native::tools
