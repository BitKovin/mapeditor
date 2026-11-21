// ui/main_window_texture -- MainWindow: the texturing commands (T11.3d). Every command is one of tools/texturing/face_edit's edits on the selection (a selected face; else all
// the faces of the selected brushes / entities) committed as one undo step. `textureCommand` is what the actions, the Surface Inspector and the tests call.
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "core/textures.h"
#include "doc/selection.h"
#include "render/viewport_window.h"
#include "tools/texturing/face_edit.h"
#include "ui/main_window.h"

namespace native::ui {

namespace tex = tools::tex;
using json = nlohmann::json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

double num(const json& p, const char* key, double fallback) {
    return p.is_object() && p.contains(key) && p[key].is_number() ? p[key].get<double>() : fallback;
}
std::string str(const json& p, const char* key, const std::string& fallback = {}) {
    return p.is_object() && p.contains(key) && p[key].is_string() ? p[key].get<std::string>() : fallback;
}
bool flag(const json& p, const char* key) { return p.is_object() && p.contains(key) && p[key].is_boolean() && p[key].get<bool>(); }

tex::NudgeMode nudgeModeOf(const std::string& s) {
    if (s == "scale") return tex::NudgeMode::Scale;
    if (s == "rotate") return tex::NudgeMode::Rotate;
    return tex::NudgeMode::Shift;
}

std::optional<tex::TexKey> texKeyOf(const std::string& s) {
    if (s == "left") return tex::TexKey::Left;
    if (s == "right") return tex::TexKey::Right;
    if (s == "up") return tex::TexKey::Up;
    if (s == "down") return tex::TexKey::Down;
    if (s == "rotcw") return tex::TexKey::RotCw;
    if (s == "rotccw") return tex::TexKey::RotCcw;
    return std::nullopt;
}

std::optional<tex::JustifyEdge> justifyOf(const std::string& s) {
    if (s == "left") return tex::JustifyEdge::Left;
    if (s == "right") return tex::JustifyEdge::Right;
    if (s == "top") return tex::JustifyEdge::Top;
    if (s == "bottom") return tex::JustifyEdge::Bottom;
    if (s == "center") return tex::JustifyEdge::Center;
    return std::nullopt;
}

} // namespace

tex::InfoFn MainWindow::textureInfo() const {
    if (textureInfo_) return textureInfo_;
    if (!texEnv_) return {};
    const auto env = texEnv_;
    return [env](const std::string& p) { return env->info(p); };
}

tex::HotspotFn MainWindow::hotspotSource() const {
    const tex::InfoFn info = textureInfo();
    const std::string root = assetsRoot_;
    const auto profile = profile_;
    if (hotspotOverride_) return hotspotOverride_;
    return [info, root, profile](const std::string& texture) -> std::optional<tex::HotspotSet> {
        if (!info || root.empty() || !profile) return std::nullopt;
        const core::texplan::TexInfo* i = info(texture);
        if (!i) return std::nullopt;
        tex::HotspotSet set;
        set.size = tex::texSizeOf(*i);
        if (const auto props = core::readTextureProps(root, *profile, texture); props && props->contains("hotspots"))
            set.hotspots = tex::parseHotspots((*props)["hotspots"]);
        return set;
    };
}

std::optional<tex::FaceRef> MainWindow::selectedFaceRef() const {
    if (!document_) return std::nullopt;
    std::optional<tex::FaceRef> found;
    for (const doc::SelRef& s : selection_->items()) {
        if (!s.faceId || !s.instancePath.empty()) continue;
        if (found) return std::nullopt;   // more than one
        found = tex::FaceRef{s.entityId, s.brushId, *s.faceId};
    }
    if (found && !tex::findFace(*document_, *found)) return std::nullopt;
    return found;
}

bool MainWindow::textureTargetsExist() const {
    return document_ && !selection_->empty() && !tex::faceTargets(*document_, selection_->items()).empty();
}

bool MainWindow::textureCommand(const std::string& op, const json& params) {
    if (!document_) return false;
    const tex::InfoFn info = textureInfo();
    const std::vector<doc::SelRef> sels = selection_->items();
    const auto say = [this](const std::string& m) { statusBar()->showMessage(qs(m), 5000); };
    if (op != "apply" && op != "lift" && !textureTargetsExist() && !(op == "copyUv" && selectedFaceRef())) {
        say("Select a brush or a face first");
        return false;
    }

    if (op == "apply") {
        const std::string texture = str(params, "texture", activeTexture_);
        if (texture.empty()) return false;
        if (params.is_object() && params.contains("texture")) activeTexture_ = texture;
        if (sels.empty()) {
            say("Active texture: " + texture);
            return true;
        }
        return applyBrushEdit(tex::applyTexture(document_, sels, texture, info));
    }
    if (op == "lift") {
        const auto face = selectedFaceRef();
        if (!face) {
            say("Lift material: select one face");
            return false;
        }
        const auto t = tex::liftMaterial(*document_, *face);
        if (!t || t->empty()) return false;
        activeTexture_ = *t;
        say("Active texture: " + *t);
        return true;
    }
    if (op == "copyUv") {
        const auto face = selectedFaceRef();
        if (!face) {
            say("Copy UV: select one face");
            return false;
        }
        copiedFace_ = *face;
        say("Copied the texture and UV of the face");
        return true;
    }
    if (op == "pasteUv") {
        if (!copiedFace_) {
            say("Paste UV: copy the UV of a face first");
            return false;
        }
        return applyBrushEdit(tex::pasteUv(document_, *copiedFace_, sels, info));
    }
    if (op == "flow") {
        if (!copiedFace_) {
            say("Flow texture: copy the UV of a face first, then select the face to flow onto");
            return false;
        }
        const auto face = selectedFaceRef();
        if (!face) {
            say("Flow texture: select the face to flow onto");
            return false;
        }
        return applyBrushEdit(tex::flowTexture(document_, *copiedFace_, *face, info));
    }
    if (op == "nudge") {
        const auto key = texKeyOf(str(params, "key"));
        if (!key) return false;
        tex::TexSteps steps;
        steps.shift = texSteps_.shift;
        steps.rotate = texSteps_.rotate;
        steps.scale = texSteps_.scale;
        return applyBrushEdit(tex::nudgeTexture(document_, sels, nudgeModeOf(str(params, "mode", "shift")), *key, flag(params, "fine"), steps, info));
    }
    if (op == "reset") return applyBrushEdit(tex::resetProjection(document_, sels, info));
    if (op == "fit") return applyBrushEdit(tex::fitTexture(document_, sels, info));
    if (op == "hotspot") return applyBrushEdit(tex::applyHotspot(document_, sels, hotspotSource()));
    if (op == "justify") {
        const auto edge = justifyOf(str(params, "edge"));
        if (!edge) return false;
        return applyBrushEdit(tex::editProjection(document_, sels, "Justify texture", [edge](const tex::Projection& p, const core::Brush& b, const core::BrushFace& f) { return tex::justify(b, f, p, *edge); }, info));
    }
    if (op == "fitAxis") {
        const int axis = num(params, "axis", 0) >= 1 ? 1 : 0;
        const double count = std::max(0.01, num(params, "count", 1));
        return applyBrushEdit(tex::editProjection(document_, sels, "Fit texture", [axis, count](const tex::Projection& p, const core::Brush& b, const core::BrushFace& f) { return tex::fitAxis(b, f, p, axis, count); }, info));
    }
    if (op == "ortho")
        return applyBrushEdit(tex::editProjection(document_, sels, "Project ortho", [](const tex::Projection& p, const core::Brush& b, const core::BrushFace& f) { return tex::projectOrtho(b, f, p); }, info));
    if (op == "axial")
        return applyBrushEdit(tex::editProjection(document_, sels, "Project axial", [](const tex::Projection& p, const core::Brush&, const core::BrushFace&) { return tex::projectAxial(p); }, info));
    if (op == "cam") {
        const render::ViewportWindow* vp = activeViewport();
        if (!vp) {
            say("Project from the camera: no viewport");
            return false;
        }
        const render::CameraBasis cb = vp->rig().active().basis();
        const tex::V3 right{cb.right.x, cb.right.y, cb.right.z}, up{cb.up.x, cb.up.y, cb.up.z};
        bool refused = false;
        const tools::BrushEdit e = tex::editProjection(document_, sels, "Project from camera", [&](const tex::Projection& p, const core::Brush& b, const core::BrushFace& f) {
            const auto r = tex::projectCam(b, f, p, right, up);
            if (!r) refused = true;
            return r ? *r : p;
        }, info);
        if (refused && !e.ok()) say("Project from the camera: the camera looks along the face");
        return applyBrushEdit(e);
    }
    if (op == "mirror") {
        const int axis = num(params, "axis", 0) >= 1 ? 1 : 0;
        return applyBrushEdit(tex::editProjection(document_, sels, "Mirror texture", [axis](const tex::Projection& p, const core::Brush&, const core::BrushFace&) { return tex::mirrorAxis(p, axis); }, info));
    }
    if (op == "shear") {
        const double shear = num(params, "value", 0);
        return applyBrushEdit(tex::editProjection(document_, sels, "Texture shear", [shear](const tex::Projection& p, const core::Brush&, const core::BrushFace&) { return tex::withShear(p, shear); }, info));
    }
    if (op == "rotate") {
        const double deg = num(params, "value", 0);
        return applyBrushEdit(tex::editProjection(document_, sels, "Rotate texture", [deg](const tex::Projection& p, const core::Brush&, const core::BrushFace&) {
            tex::Projection r = p;
            r.rotate = deg;
            return r;
        }, info));
    }
    if (op == "shift") {   // value in texels
        const int axis = num(params, "axis", 0) >= 1 ? 1 : 0;
        const double texels = num(params, "value", 0);
        return applyBrushEdit(tex::editProjection(document_, sels, "Shift texture", [axis, texels, &info](const tex::Projection& p, const core::Brush&, const core::BrushFace& f) {
            return tex::withShiftTexels(p, axis, texels, tex::surfaceBasis(f, info).texels);
        }, info));
    }
    if (op == "stretch") {   // value = a multiple of the natural size
        const int axis = num(params, "axis", 0) >= 1 ? 1 : 0;
        const double value = num(params, "value", 1);
        return applyBrushEdit(tex::editProjection(document_, sels, "Stretch texture", [axis, value, &info](const tex::Projection& p, const core::Brush&, const core::BrushFace& f) {
            return tex::withStretch(p, axis, value, tex::surfaceBasis(f, info).natural);
        }, info));
    }
    if (op == "matchGrid") {
        const render::ViewportWindow* vp = activeViewport();
        const double grid = num(params, "grid", vp ? vp->rig().settings().gridSize : 0.25);
        return applyBrushEdit(tex::editProjection(document_, sels, "Match grid", [grid](const tex::Projection& p, const core::Brush&, const core::BrushFace&) { return tex::matchGrid(p, grid); }, info));
    }
    if (op == "lightmap") {   // {"factor": 2} multiplies, {"value": 1} sets
        const bool set = params.is_object() && params.contains("value");
        const double v = set ? num(params, "value", 1) : num(params, "factor", 1);
        return applyBrushEdit(tex::editLightmapScale(document_, sels, "Lightmap density", [set, v](double now) { return set ? v : now * v; }));
    }
    if (op == "repeat") {   // {axis, mode?: "repeat"|"mirror"|"", edge?: "wrap"|"clamp"|""}
        tex::RepeatPatch patch;
        if (params.is_object() && params.contains("mode")) {
            patch.setMode = true;
            patch.mode = str(params, "mode");
        }
        if (params.is_object() && params.contains("edge")) {
            patch.setEdge = true;
            patch.edge = str(params, "edge");
        }
        return applyBrushEdit(tex::setFaceRepeat(document_, sels, num(params, "axis", 0) >= 1 ? 1 : 0, patch));
    }
    say("Unknown texture command: " + op);
    return false;
}

} // namespace native::ui
