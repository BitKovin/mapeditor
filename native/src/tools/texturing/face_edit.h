// tools/texturing/face_edit -- the texturing actions on the document (ports of the texturing part of shell/src/state/actions.ts): apply a texture, edit the projection of the
// target faces, lightmap density, copy / paste surface, flow, hotspot, face repetition and the Radiant texture keys. Each takes the version and the selection and returns the next
// version (a tools::BrushEdit); the caller commits it as one undo step. A no-op edit gives no version and no message ("nothing changed", not an error).
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (state/actions.ts), 08_MILESTONES.md M11 item 3.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/brush_edit.h"
#include "tools/texturing/hotspots.h"
#include "tools/texturing/surface.h"

namespace native::tools::tex {

struct FaceRef {
    std::string entityId, brushId;
    uint32_t faceId = 0;
    bool operator==(const FaceRef& o) const { return entityId == o.entityId && brushId == o.brushId && faceId == o.faceId; }
};

/// What texture edits apply to: a selected face; else every face of the selected brush / entity. Prefab content is skipped.
std::vector<FaceRef> faceTargets(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels);
/// The face a reference names, null when it is gone.
const core::BrushFace* findFace(const doc::DocumentVersion& doc, const FaceRef& ref);
/// The brush a face reference lies in, null when it is gone.
const core::Brush* findBrushOf(const doc::DocumentVersion& doc, const FaceRef& ref);

using FaceFn = std::function<core::BrushFace(const core::Brush&, const core::BrushFace&)>;
/// `fn` on every target face; a face that comes back unchanged (same uvs / look / texture) is not an edit. No version when nothing changed.
BrushEdit mapFaces(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<FaceRef>& targets, const std::string& label, const FaceFn& fn);

/// Applies the texture to the target faces: a texture replaces a texture group layout; a face without UVs gets the texture at its natural size from its bottom-left corner;
/// otherwise the face keeps its stretch (scale relative to the natural size), shift in texels, rotation and shear.
BrushEdit applyTexture(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& texture, const InfoFn& info);
/// `change` on each target face's projection; the uvs are rewritten with it.
using ProjectionFn = std::function<Projection(const Projection&, const core::Brush&, const core::BrushFace&)>;
BrushEdit editProjection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& label, const ProjectionFn& change, const InfoFn& info);
BrushEdit resetProjection(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const InfoFn& info);
BrushEdit fitTexture(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const InfoFn& info);
/// Lightmap texel density of the target faces = change(current), clamped (1 is stored as absent).
BrushEdit editLightmapScale(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& label, const std::function<double(double)>& change);
double lightmapScaleOf(const core::BrushFace& f);

/// Copies the texture and the texturing parameters of face `from` onto face `to` and recomputes the target's uvs. Refused for the same face or a missing one.
BrushEdit copySurface(const std::shared_ptr<const doc::DocumentVersion>& base, const FaceRef& from, const FaceRef& to, const InfoFn& info);
/// Paste UV: the copied face's texture and mapping onto every target face (whole brushes: all their faces), one edit. `count` = the number of target faces.
BrushEdit pasteUv(const std::shared_ptr<const doc::DocumentVersion>& base, const FaceRef& from, const std::vector<doc::SelRef>& sels, const InfoFn& info, size_t* count = nullptr);
/// Flow texture: the UVs of `from` continue onto `to` (and the faces between) across shared edges, within one brush.
BrushEdit flowTexture(const std::shared_ptr<const doc::DocumentVersion>& base, const FaceRef& from, const FaceRef& to, const InfoFn& info);
/// Lift Material: the texture of a face; nullopt when the face does not exist.
std::optional<std::string> liftMaterial(const doc::DocumentVersion& doc, const FaceRef& ref);

/// The hotspots of a texture (null / empty = none) and its size; supplied by the texture properties.
struct HotspotSet {
    std::vector<Hotspot> hotspots;
    TexSize size;
};
using HotspotFn = std::function<std::optional<HotspotSet>(const std::string& texture)>;
/// Every target face shows the hotspot of its texture that fits its size best. The message says why when nothing was done.
BrushEdit applyHotspot(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const HotspotFn& hotspots);

/// Sets (or clears, nullopt) one axis' repeat mode ("repeat" / "mirror") and edge ("wrap" / "clamp") override on every target face (`look.repeat`). `setMode` / `setEdge`
/// say which of the two is touched: a touched one is cleared when its value is empty.
struct RepeatPatch {
    bool setMode = false, setEdge = false;
    std::string mode, edge;   // empty = clear
};
BrushEdit setFaceRepeat(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, int axis, const RepeatPatch& patch);

/// Texture groups (state/actions.ts applyTexGroup): the target faces are laid out by the group (`look.layout = {group}`); the face keeps its texture as the fallback, or takes
/// `fallbackTexture` (the group's fallback slot). `wallsOnly` leaves out the roof / floor faces of whole brushes (a face picked by hand always gets the group).
BrushEdit applyTexGroup(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& group, const std::string& fallbackTexture, bool wallsOnly);
/// `change` on the layout JSON of every target face that has one (null / an empty object removes it); the other faces are left alone.
BrushEdit editLayout(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& label, const std::function<nlohmann::json(const nlohmann::json&)>& change);
/// Back to a plain face (the face keeps its texture).
BrushEdit clearLayout(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels);
/// The group name of a face's layout ("" = none).
std::string layoutGroupOf(const core::BrushFace& f);

/// The Radiant texture keys: Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp/PageDown rotate; `fine` divides the step by 8.
struct TexSteps {
    double shift = 0.125, rotate = 15, scale = 0.1;
};
enum class TexKey { Left, Right, Up, Down, RotCw, RotCcw };
enum class NudgeMode { Shift, Scale, Rotate };
BrushEdit nudgeTexture(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, NudgeMode mode, TexKey key, bool fine, const TexSteps& steps, const InfoFn& info);

} // namespace native::tools::tex
