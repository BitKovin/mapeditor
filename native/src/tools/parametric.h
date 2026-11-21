// tools/parametric -- parametric shapes (port of shell/src/model/parametric.ts, REQUIREMENTS 22 P1). Every brush of a placed cylinder / wedge / arch / stairs / sphere carries a
// `shape` record in its JSON (the brush node's raw): the group it belongs to, its part number, and how the whole shape was made (kind, parameters, the drawn box, orientation).
// The Details panel edits the record and every part is rebuilt in place; transforms carry the box along (`mapShapeRecord`); any other change to a part's geometry turns the
// group into ordinary brushes (`reconcileShapes`, run on every committed edit). The Core keeps the record as it keeps any key it does not know.
// Thread: any (pure on immutable versions).
// Spec: 07_EDITOR_PORT.md B (parametric.ts), 08_MILESTONES.md M11 item 6.
#pragma once

#include <core/texplan.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/edit.h"
#include "tools/shapes.h"

namespace native::tools::parametric {

using Json = nlohmann::json;

/// The kinds a record may name (everything but the box).
bool isParametric(ShapeId id);
std::optional<ShapeId> kindFromName(const std::string& name);

struct ShapeSpec {
    std::string group;
    ShapeId kind = ShapeId::Cylinder;
    int part = 0, parts = 1;
    Frame frame;
    Json params = Json::object();   // only the parameters that matter for the kind
    Turn turn = 0;
};

/// The parameters of `kind` that are stored (the rest of ShapeParams is not).
Json paramsFor(ShapeId kind, const ShapeParams& p);
/// ShapeParams with the stored keys applied over the defaults.
ShapeParams paramsFrom(const Json& stored, Turn turn = 0);

/// The `shape` record of one part.
Json shapeRecord(const ShapeSpec& s);
/// A brush JSON's `shape` record, or nullopt when there is none (or a damaged one).
std::optional<ShapeSpec> shapeOf(const Json& rawBrush);
inline std::optional<ShapeSpec> shapeOf(const doc::BrushNode& n) { return shapeOf(n.raw); }

/// The box after a point map (an affine map keeps every generated point where the mapped brush has it, so a moved shape still matches).
Frame mapFrame(const Frame& f, const std::function<Vec3(const Vec3&)>& map);
/// The record with its box mapped (the same record when it is damaged or absent: then `raw` is returned unchanged).
Json mapShapeRecord(const Json& rawShape, const std::function<Vec3(const Vec3&)>& map);

/// The parts a spec describes: geometry only, texture "", ids p0, p1... (or the error text).
ShapeOutcome partsOf(ShapeId kind, const Frame& frame, const Json& params, Turn turn);

/// Adds freshly built `brushes` to the entity as one shape (every brush gets its record). False when an add was refused (the editor keeps the error).
bool addShape(doc::VersionEditor& ed, const std::string& entityId, const std::vector<core::Brush>& brushes, ShapeId kind, const Frame& frame, const ShapeParams& params, Turn turn,
              const std::string& group);

struct ShapePatch {
    Json params = Json::object();   // keys to change
    std::optional<double> du, dv, h;
    std::optional<Turn> turn;
};
struct Regenerated {
    std::shared_ptr<const doc::DocumentVersion> version;   // null on error
    std::string error;
    bool ok() const { return error.empty() && version; }
};
/// Rebuilds the shape `group` of entity `entityId` with `patch` applied: part i keeps the id of the old part i (extra parts get `newId()`, surplus parts are removed), faces keep
/// their look by face id (stored UVs dropped, so the projection applies; `info` re-aligns when given). The old shape is untouched on error.
Regenerated regenerateShape(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& entityId, const std::string& group, const ShapePatch& patch,
                            const std::function<std::string()>& newId, const core::texplan::InfoFn& info = {});

/// Keeps records honest after an edit (`prev` -> `next`): a group must sit whole in one entity (every part index once, the same kind and part count), and every part that changed
/// must still be exactly what its record builds; otherwise the record is dropped from the whole group (the shape becomes ordinary brushes). Parts that were not in the group
/// before (a duplicate) get a new group id from `freshGroup`. Returns `next` itself when nothing had to change.
std::shared_ptr<const doc::DocumentVersion> reconcileShapes(const std::shared_ptr<const doc::DocumentVersion>& prev, const std::shared_ptr<const doc::DocumentVersion>& next,
                                                            const std::function<std::string()>& freshGroup);

/// The shape turned into ordinary brushes (the record dropped from every part); the version itself when `group` is not there.
std::shared_ptr<const doc::DocumentVersion> dissolveShape(const std::shared_ptr<const doc::DocumentVersion>& v, const std::string& entityId, const std::string& group);

/// Ids of every part of the shape `brushId` belongs to (in part order), or just {brushId} when it is no shape.
std::vector<std::string> shapeMembers(const doc::DocumentVersion& v, const std::string& entityId, const std::string& brushId);

} // namespace native::tools::parametric
