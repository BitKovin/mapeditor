// tools/texturing/face_edit -- see face_edit.h.
#include "tools/texturing/face_edit.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "doc/edit.h"
#include "tools/texturing/flow.h"

namespace native::tools::tex {

using doc::SelRef;
using nlohmann::json;
using Base = std::shared_ptr<const doc::DocumentVersion>;
namespace tp = core::texplan;

namespace {

BrushEdit refused(const std::string& message) {
    BrushEdit r;
    r.message = message;
    return r;
}

bool sameFace(const core::BrushFace& a, const core::BrushFace& b) {
    if (a.texture != b.texture || a.lookHash != b.lookHash || a.lightmapScale != b.lightmapScale || a.uvs.size() != b.uvs.size() || a.loop != b.loop) return false;
    if (a.projection.scaleU != b.projection.scaleU || a.projection.scaleV != b.projection.scaleV || a.projection.shiftU != b.projection.shiftU ||
        a.projection.shiftV != b.projection.shiftV || a.projection.rotate != b.projection.rotate)
        return false;
    for (size_t i = 0; i < a.uvs.size(); ++i)
        if (a.uvs[i].u != b.uvs[i].u || a.uvs[i].v != b.uvs[i].v) return false;
    const json empty = json::object();
    const json& la = a.look ? *a.look : empty;
    const json& lb = b.look ? *b.look : empty;
    return la == lb;
}

const tp::TexInfo* infoOf(const InfoFn& info, const std::string& texture) { return info ? info(texture.empty() ? "dev/grey" : texture) : nullptr; }

// the face without its texture group layout
core::BrushFace withoutLayout(const core::BrushFace& f) {
    core::BrushFace nf = f;
    if (!f.look || !f.look->is_object() || !f.look->contains("layout")) return nf;
    json look = *f.look;
    look.erase("layout");
    if (look.empty()) nf.look.reset();
    else nf.look = std::make_shared<const json>(std::move(look));
    refreshLookHash(nf);
    return nf;
}

core::BrushFace withLook(const core::BrushFace& f, const std::function<void(json&)>& change) {
    core::BrushFace nf = f;
    json look = f.look && f.look->is_object() ? *f.look : json::object();
    change(look);
    if (look.empty()) nf.look.reset();
    else nf.look = std::make_shared<const json>(std::move(look));
    refreshLookHash(nf);
    return nf;
}

} // namespace

std::vector<FaceRef> faceTargets(const doc::DocumentVersion& d, const std::vector<SelRef>& sels) {
    std::vector<FaceRef> out;
    std::set<std::string> seen;
    for (const SelRef& s : sels) {
        if (s.isInstance() || !s.instancePath.empty() || s.entityId.empty()) continue;
        const int ei = d.findEntity(s.entityId);
        if (ei < 0) continue;
        for (const auto& node : d.entities[static_cast<size_t>(ei)]->brushes) {
            if (!s.brushId.empty() && node->brush.id != s.brushId) continue;
            for (const core::BrushFace& f : node->brush.faces) {
                if (s.faceId && f.id != *s.faceId) continue;
                if (seen.insert(s.entityId + "/" + node->brush.id + "/" + std::to_string(f.id)).second) out.push_back({s.entityId, node->brush.id, f.id});
            }
        }
    }
    return out;
}

const core::Brush* findBrushOf(const doc::DocumentVersion& d, const FaceRef& ref) {
    const int ei = d.findEntity(ref.entityId);
    if (ei < 0) return nullptr;
    for (const auto& node : d.entities[static_cast<size_t>(ei)]->brushes)
        if (node->brush.id == ref.brushId) return &node->brush;
    return nullptr;
}

const core::BrushFace* findFace(const doc::DocumentVersion& d, const FaceRef& ref) {
    const core::Brush* b = findBrushOf(d, ref);
    if (!b) return nullptr;
    for (const core::BrushFace& f : b->faces)
        if (f.id == ref.faceId) return &f;
    return nullptr;
}

BrushEdit mapFaces(const Base& base, const std::vector<FaceRef>& targets, const std::string& label, const FaceFn& fn) {
    if (!base) return refused("no document");
    std::map<std::pair<std::string, std::string>, std::set<uint32_t>> byBrush;
    std::vector<std::pair<std::string, std::string>> order;
    for (const FaceRef& t : targets) {
        const auto key = std::make_pair(t.entityId, t.brushId);
        if (!byBrush.count(key)) order.push_back(key);
        byBrush[key].insert(t.faceId);
    }
    doc::VersionEditor ed(base);
    bool any = false;
    for (const auto& key : order) {
        const core::Brush* b = findBrushOf(*base, {key.first, key.second, 0});
        if (!b) continue;
        core::Brush nb = *b;
        bool changed = false;
        for (size_t i = 0; i < nb.faces.size(); ++i) {
            if (!byBrush[key].count(b->faces[i].id)) continue;
            core::BrushFace nf = fn(*b, b->faces[i]);
            if (sameFace(nf, b->faces[i])) continue;
            nb.faces[i] = std::move(nf);
            changed = true;
        }
        if (!changed) continue;
        ed.replaceBrush(key.first, nb);
        any = true;
    }
    if (!any) return {};
    BrushEdit r;
    r.label = label;
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    return r;
}

BrushEdit applyTexture(const Base& base, const std::vector<SelRef>& sels, const std::string& texture, const InfoFn& info) {
    if (!base) return refused("no document");
    const std::vector<FaceRef> targets = faceTargets(*base, sels);
    if (targets.empty()) return {};
    return mapFaces(base, targets, "Apply texture", [&](const core::Brush& b, const core::BrushFace& face) {
        const core::BrushFace f = withoutLayout(face);   // a texture replaces a texture group: the layout would keep drawing over it
        if (f.texture == texture) return f;
        const tp::TexInfo* now = infoOf(info, texture);
        // a face without any UVs gets the texture at its natural size from its bottom-left corner
        if (!hasProjection(f) && f.uvs.empty() && now) {
            core::BrushFace t = f;
            t.texture = texture;
            return withProjection(b, t, bottomLeft(b, t, defaultProjectionFor(texture, info)));
        }
        // otherwise the face keeps its Texture panel values: stretch (scale / the texture's natural size), shift in texels, rotation, shear
        const tp::TexInfo* was = infoOf(info, f.texture);
        if (was && now && was->width > 0 && was->height > 0 && now->width > 0 && now->height > 0) {
            const V2 an = tp::naturalSize(*was), nn = tp::naturalSize(*now);
            const double kx = nn[0] / an[0], ky = nn[1] / an[1];
            if (hasProjection(f)) {
                Projection p = projectionOfFace(f, info);
                p.scale = {p.scale[0] * kx, p.scale[1] * ky};
                p.shift = {p.shift[0] * was->width / now->width, p.shift[1] * was->height / now->height};
                core::BrushFace t = f;
                t.texture = texture;
                return withProjection(b, t, p);
            }
            if (!f.uvs.empty()) {
                core::BrushFace t = f;
                t.texture = texture;
                for (core::Vec2& uv : t.uvs) {
                    uv.u /= kx;
                    uv.v /= ky;
                }
                refreshLookHash(t);
                return t;
            }
        }
        core::BrushFace t = f;
        t.texture = texture;
        return t;
    });
}

BrushEdit editProjection(const Base& base, const std::vector<SelRef>& sels, const std::string& label, const ProjectionFn& change, const InfoFn& info) {
    if (!base) return refused("no document");
    const std::vector<FaceRef> targets = faceTargets(*base, sels);
    if (targets.empty()) return {};
    return mapFaces(base, targets, label, [&](const core::Brush& b, const core::BrushFace& f) { return withProjection(b, f, change(projectionOfFace(f, info), b, f)); });
}

BrushEdit resetProjection(const Base& base, const std::vector<SelRef>& sels, const InfoFn& info) {
    return editProjection(base, sels, "Reset texture", [&](const Projection&, const core::Brush& b, const core::BrushFace& f) { return resetSurface(b, f, info); }, info);
}

BrushEdit fitTexture(const Base& base, const std::vector<SelRef>& sels, const InfoFn& info) {
    return editProjection(base, sels, "Fit texture", [](const Projection& p, const core::Brush& b, const core::BrushFace& f) { return fitProjection(b, f, p); }, info);
}

double lightmapScaleOf(const core::BrushFace& f) {
    return std::isfinite(f.lightmapScale) && f.lightmapScale > 0 ? std::min(core::kMaxLightmapScale, std::max(core::kMinLightmapScale, f.lightmapScale)) : 1;
}

BrushEdit editLightmapScale(const Base& base, const std::vector<SelRef>& sels, const std::string& label, const std::function<double(double)>& change) {
    if (!base) return refused("no document");
    const std::vector<FaceRef> targets = faceTargets(*base, sels);
    if (targets.empty()) return {};
    return mapFaces(base, targets, label, [&](const core::Brush&, const core::BrushFace& f) {
        const double v = std::min(core::kMaxLightmapScale, std::max(core::kMinLightmapScale, change(lightmapScaleOf(f))));
        if (v == lightmapScaleOf(f)) return f;
        core::BrushFace nf = f;
        nf.lightmapScale = v;   // 1 is written as absent by the document writer
        return nf;
    });
}

BrushEdit copySurface(const Base& base, const FaceRef& from, const FaceRef& to, const InfoFn& info) {
    if (!base) return refused("no document");
    if (from == to) return refused("Copy UV: pick a different face");
    const core::BrushFace* src = findFace(*base, from);
    if (!src || !findFace(*base, to)) return refused("Copy UV: the face is gone");
    const Projection p = projectionOfFace(*src, info);
    const std::string texture = src->texture;
    BrushEdit r = mapFaces(base, {to}, "Copy UV", [&](const core::Brush& b, const core::BrushFace& f) {
        core::BrushFace t = f;
        t.texture = texture;
        return withProjection(b, t, p);
    });
    return r;
}

BrushEdit pasteUv(const Base& base, const FaceRef& from, const std::vector<SelRef>& sels, const InfoFn& info, size_t* count) {
    if (count) *count = 0;
    if (!base) return refused("no document");
    const core::BrushFace* src = findFace(*base, from);
    if (!src) return refused("Paste UV: copy a face first (the copied face no longer exists)");
    std::vector<FaceRef> targets;
    for (const FaceRef& t : faceTargets(*base, sels))
        if (!(t == from)) targets.push_back(t);
    if (targets.empty()) return {};
    if (count) *count = targets.size();
    const Projection p = projectionOfFace(*src, info);
    const std::string texture = src->texture;
    return mapFaces(base, targets, "Paste UV", [&](const core::Brush& b, const core::BrushFace& f) {
        core::BrushFace t = f;
        t.texture = texture;
        return withProjection(b, t, p);
    });
}

BrushEdit flowTexture(const Base& base, const FaceRef& from, const FaceRef& to, const InfoFn& info) {
    if (!base) return refused("no document");
    if (from.entityId != to.entityId || from.brushId != to.brushId) return refused("Flow works between faces of the same brush");
    if (from.faceId == to.faceId) return refused("Pick a different face to flow onto");
    const core::Brush* b = findBrushOf(*base, from);
    if (!b) return refused("Face not found");
    const auto r = flowFaces(*b, from.faceId, {to.faceId}, info);
    if (!r) return refused("The faces are not connected through shared edges");
    doc::VersionEditor ed(base);
    ed.replaceBrush(from.entityId, r->brush);
    BrushEdit out;
    out.label = "Flow texture";
    out.version = ed.finish();
    if (!out.version) return refused(ed.error());
    return out;
}

std::optional<std::string> liftMaterial(const doc::DocumentVersion& d, const FaceRef& ref) {
    const core::BrushFace* f = findFace(d, ref);
    if (!f) return std::nullopt;
    return f->texture.empty() ? "dev/grey" : f->texture;
}

BrushEdit applyHotspot(const Base& base, const std::vector<SelRef>& sels, const HotspotFn& hotspots) {
    if (!base) return refused("no document");
    const std::vector<FaceRef> targets = faceTargets(*base, sels);
    if (targets.empty()) return refused("Select a face or a brush first");
    size_t hit = 0;
    BrushEdit r = mapFaces(base, targets, "Apply hotspot", [&](const core::Brush& b, const core::BrushFace& f) {
        const auto set = hotspots ? hotspots(f.texture) : std::nullopt;
        if (!set || set->hotspots.empty()) return f;
        const int k = chooseHotspot(set->hotspots, set->size, faceSize(b, f));
        if (k < 0) return f;
        ++hit;
        return withProjection(b, f, hotspotProjection(b, f, set->hotspots[static_cast<size_t>(k)], set->size));
    });
    if (!hit) return refused("The texture has no hotspots yet: draw them in the texture properties (double-click the texture in the browser)");
    return r;
}

BrushEdit setFaceRepeat(const Base& base, const std::vector<SelRef>& sels, int axis, const RepeatPatch& patch) {
    if (!base) return refused("no document");
    const std::vector<FaceRef> targets = faceTargets(*base, sels);
    if (targets.empty()) return {};
    const char* key = axis == 0 ? "u" : "v";
    return mapFaces(base, targets, "Face repetition", [&](const core::Brush&, const core::BrushFace& f) {
        return withLook(f, [&](json& look) {
            json repeat = look.contains("repeat") && look["repeat"].is_object() ? look["repeat"] : json::object();
            json a = repeat.contains(key) && repeat[key].is_object() ? repeat[key] : json::object();
            if (patch.setMode) {
                if (patch.mode.empty()) a.erase("mode");
                else a["mode"] = patch.mode;
            }
            if (patch.setEdge) {
                if (patch.edge.empty()) a.erase("edge");
                else a["edge"] = patch.edge;
            }
            if (a.empty()) repeat.erase(key);
            else repeat[key] = a;
            if (repeat.empty()) look.erase("repeat");
            else look["repeat"] = repeat;
        });
    });
}

BrushEdit nudgeTexture(const Base& base, const std::vector<SelRef>& sels, NudgeMode mode, TexKey key, bool fine, const TexSteps& steps, const InfoFn& info) {
    const double f = fine ? 1.0 / 8 : 1.0;
    const bool positive = key == TexKey::Left || key == TexKey::Up || key == TexKey::RotCw;
    const double dir = positive ? 1 : -1;
    const bool horizontal = key == TexKey::Left || key == TexKey::Right;
    if (mode == NudgeMode::Shift) {
        const double d = steps.shift * f * dir;
        return editProjection(base, sels, "Shift texture", [=](const Projection& p, const core::Brush&, const core::BrushFace&) {
            Projection q = p;
            if (horizontal) q.shift[0] += d;
            else q.shift[1] += d;
            return q;
        }, info);
    }
    if (mode == NudgeMode::Scale) {
        const bool grow = key == TexKey::Right || key == TexKey::Up;
        const double k = grow ? 1 + steps.scale * f : 1 / (1 + steps.scale * f);
        return editProjection(base, sels, "Scale texture", [=](const Projection& p, const core::Brush&, const core::BrushFace&) {
            Projection q = p;
            if (horizontal) q.scale[0] *= k;
            else q.scale[1] *= k;
            return q;
        }, info);
    }
    const double d = steps.rotate * f * (key == TexKey::RotCw ? 1 : -1);
    return editProjection(base, sels, "Rotate texture", [=](const Projection& p, const core::Brush&, const core::BrushFace&) {
        Projection q = p;
        q.rotate += d;
        return q;
    }, info);
}

} // namespace native::tools::tex
