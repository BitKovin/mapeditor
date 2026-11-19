// tools/transform -- grab / rotate / scale of brushes, entities and prefab instances (port of shell/src/model/transform.ts).
//
// Owns: Affine (a point map: rotation about a pivot, scale about a pivot, a translation), mapBrushPoints (with UV lock), mapEntityProps (what a rotate / scale does
// to a point entity besides moving it: its `angles`, light radius / width / height, decal size, model scale), mapEntityPoints, mapInstancePoints, selectionTargets,
// transformSelections (the edited DocumentVersion through a VersionEditor: only touched nodes are replaced) and selectionPivot. The viewport computes a parameter
// from the mouse (or typed digits) and asks for the result on every move, always from the document as it was when the operation began.
// Thread: UI thread / any (pure on immutable versions).
// Spec: 08_MILESTONES.md T4.1, DESIGN sections 8 and 17.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/document.h"
#include "doc/document.h"
#include "doc/selection.h"
#include "scene/point_box.h"

namespace native::tools {

using scene::Mat3;
using scene::V3;

// p' = m * p + t
struct Affine {
    Mat3 m{1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major
    V3 t{0, 0, 0};
    V3 operator()(const V3& p) const;
};
Affine rotateAbout(const V3& pivot, const V3& axis, double angle);   // Rodrigues: the line through `pivot` with direction `axis`
Affine scaleAbout(const V3& pivot, const V3& factors);
Affine translateBy(const V3& d);
// Scaling by a negative factor along an odd number of axes mirrors the mesh, which turns it inside out.
bool mirrors(const V3& factors);

Mat3 mul3(const Mat3& a, const Mat3& b);
double det3(const Mat3& m);
V3 anglesFromRotation(const Mat3& r);   // [pitch, yaw, roll] in degrees (inverse of scene::rotationFromAngles)

// Maps a brush's points. With `lockUv` (the default; the toolbar's UV lock) every face keeps the UVs it has now, stored per corner, so the texture travels with the
// face. Without it the world-aligned projection is applied again at the new place. `flip` reverses every face loop (a mirroring map turns the mesh inside out).
core::Brush mapBrushPoints(const core::Brush& b, const Affine& map, bool flip = false, bool lockUv = true);
// What a rotate / scale does to a POINT entity besides moving it, read off the linear part of the map (angles; light radius / source_size / width / height;
// decal width / height / depth along its own axes; model scale). The origin is not touched.
core::Entity mapEntityProps(const core::Entity& e, const Affine& map);
// The entity moved by the map: its origin, and for a point entity its props; a brush entity (`hasBrushes`) only turns its yaw by `yawDeg` when asked to.
core::Entity mapEntityPoints(const core::Entity& e, bool hasBrushes, const Affine& map, double yawDeg = 0);
// Moves a prefab instance by an affine map: the origin always, a rotation composes into `angles`, a pure scale multiplies `scale` when expressible.
core::PrefabInstance mapInstancePoints(const core::PrefabInstance& p, const Affine& map);

// What a set of selections covers: whole entities, brushes whose entity is not itself selected, and prefab instances. Selections inside an instance are not
// transformable (their overrides come with the prefab tools).
struct Targets {
    std::set<std::string> entities;
    std::map<std::string, std::set<std::string>> brushes;
    std::set<std::string> instances;
    bool empty() const { return entities.empty() && brushes.empty() && instances.empty(); }
};
Targets selectionTargets(const std::vector<doc::SelRef>& sels);

// Applies the map to everything the selections cover. Returns `base` itself when nothing is covered, null when an edit was refused (invalid brush). `yawDeg` also
// turns the angles of brush entities.
std::shared_ptr<const doc::DocumentVersion> transformSelections(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels,
                                                                const Affine& map, bool flip = false, double yawDeg = 0, bool lockUv = true);
// The same, moving only the origin of one entity (the origin handle of a brush entity): its brushes stay.
std::shared_ptr<const doc::DocumentVersion> moveEntityOrigin(const std::shared_ptr<const doc::DocumentVersion>& base, const std::string& entityId, const V3& delta);

// Where the gizmo sits: for one selection the entity origin of a point / custom-origin entity or the bounds centre of a brush / entity, an instance's origin;
// for several the centre of their bounds. Nullopt when nothing is covered.
std::optional<V3> gizmoPivot(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);
// Centre of the selections' bounds; an entity without brushes contributes its origin. Nullopt when nothing is covered.
std::optional<V3> selectionPivot(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);

} // namespace native::tools
