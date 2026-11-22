// tools/entity_edit -- the Details panel's edits and entity placement as pure functions (ports of shell/src/state/actions.ts: setKeyvalue, setOrigin, setClassname,
// setEntityAngle / Scale / Ref, setBrushOp / Carve, addPointEntity, spawnInstance, renameInstance, setInstanceVec, the per-instance overrides). Each takes the
// document version and returns the next one (EditResult, committed as one undo step by the caller); nothing changes when the value already is what is asked for.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tools/edit_ops.h"

namespace native::tools {

using BasePtr = std::shared_ptr<const doc::DocumentVersion>;

// ---- entities ----
EditResult setKeyvalue(const BasePtr& base, const std::string& entityId, const std::string& key, const std::optional<std::string>& value);   // nullopt removes the key
EditResult setClassname(const BasePtr& base, const std::string& entityId, const std::string& classname);   // refused for convex-only classes with a concave brush
EditResult setOrigin(const BasePtr& base, const std::string& entityId, int axis, double value);
EditResult setAngle(const BasePtr& base, const std::string& entityId, int axis, double value);
// Turns the entity to `angles` in one edit (the angle handle's drag); nothing changes when they already are.
EditResult setAngles(const BasePtr& base, const std::string& entityId, const core::Vec3& angles);
// The angles (degrees) that make an entity look along `dir` (forward = R(angles) * (0, 0, 1) = (sin yaw cos pitch, -sin pitch, cos yaw cos pitch)); the roll of `current` is kept.
// `keepPitch`: only the yaw follows (a drag on the horizontal plane). `snapDeg` > 0 rounds yaw and pitch to that step. Empty for a zero direction.
std::optional<core::Vec3> anglesToward(const core::Vec3& current, const core::Vec3& dir, bool keepPitch, double snapDeg = 0);
EditResult setScale(const BasePtr& base, const std::string& entityId, int axis, double value);   // writes modelscale_vec; the key goes at 1 1 1
// Points reference `key` at `paths` (root-relative target paths; several = entities that share one name); an empty list removes it.
EditResult setRef(const BasePtr& base, const std::string& entityId, const std::string& key, const std::vector<std::string>& paths);
// Gives the document entities at `paths` (ids; targets inside prefab instances are skipped) the targetname of the first one.
EditResult shareTargetName(const BasePtr& base, const std::vector<std::string>& paths);
EditResult setBrushOp(const BasePtr& base, const std::string& entityId, const std::string& brushId, bool subtract);
enum class CarveKey { Self, Structural, NonStructural };
EditResult setCarve(const BasePtr& base, const std::string& entityId, const std::string& brushId, CarveKey key, bool value);

// The default angles of a new light: spot, directional and rect lights point straight down (90 0 0).
std::optional<std::array<double, 3>> defaultLightAngles(const std::string& classname);
// A new point entity (selected afterwards).
EditResult addPointEntity(const BasePtr& base, const std::string& classname, const core::Vec3& origin, const std::vector<std::pair<std::string, std::string>>& keyvalues = {},
                          const std::optional<core::Vec3>& angles = std::nullopt);

// The angles (degrees, pitch yaw roll 0) of a decal that projects INTO a surface whose outward normal is `normal` (core/decal.h: dir = Ry(yaw) Rx(pitch) * (0,-1,0)).
core::Vec3 decalAnglesFor(const core::Vec3& normal);
// A new decal projector (class `decal`) lying on a surface at `point` with that outward normal; selected afterwards. Sizes in metres.
EditResult addDecal(const BasePtr& base, const std::string& texture, const core::Vec3& point, const core::Vec3& normal, double width, double height, double depth);

// ---- prefab instances ----
EditResult spawnInstance(const BasePtr& base, const std::string& file, const core::Vec3& origin);
EditResult renameInstance(const BasePtr& base, const std::string& instanceId, const std::string& name);   // refused: empty, '|' or '/' in it, or used by another instance
enum class InstanceVec { Origin, Angles, Scale };
EditResult setInstanceVec(const BasePtr& base, const std::string& instanceId, InstanceVec field, int axis, double value);
EditResult setOverrideKeyvalue(const BasePtr& base, const std::string& instanceId, const std::string& path, const std::string& key, const std::optional<std::string>& value);
enum class OverrideField { Origin, Angles, Scale, Deleted };
// Sets (value = three numbers; Deleted: any) or reverts (nullopt) a transform / deleted override of one entity of the instance.
EditResult setOverrideField(const BasePtr& base, const std::string& instanceId, const std::string& path, OverrideField field, const std::optional<std::array<double, 3>>& value);
EditResult setOverrideRef(const BasePtr& base, const std::string& instanceId, const std::string& path, const std::string& key, const std::vector<std::string>& paths);
EditResult revertOverride(const BasePtr& base, const std::string& instanceId, const std::string& path);

} // namespace native::tools
