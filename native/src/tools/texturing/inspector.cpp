// tools/texturing/inspector -- see inspector.h.
#include "tools/texturing/inspector.h"

#include <cmath>

#include "tools/texturing/density.h"

namespace native::tools::tex {

namespace {

namespace tp = core::texplan;

std::string modeName(const std::optional<tp::AxisMode>& m) {
    if (!m) return {};
    return *m == tp::AxisMode::Mirror ? "mirror" : *m == tp::AxisMode::Repeat ? "repeat" : std::string();
}
std::string edgeName(const std::optional<tp::EdgeMode>& e) {
    if (!e) return {};
    return *e == tp::EdgeMode::Wrap ? "wrap" : *e == tp::EdgeMode::Clamp ? "clamp" : std::string();
}

bool near(const V2& a, const V2& b) { return std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9; }

} // namespace

SurfaceFields surfaceFields(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels, const InfoFn& info) {
    SurfaceFields out;
    const std::vector<FaceRef> targets = faceTargets(doc, sels);
    if (targets.empty()) return out;
    out.faceCount = targets.size();
    const core::BrushFace* first = nullptr;
    const core::Brush* firstBrush = nullptr;
    Projection firstProj;
    for (const FaceRef& t : targets) {
        const core::BrushFace* f = findFace(doc, t);
        const core::Brush* b = findBrushOf(doc, t);
        if (!f || !b) continue;
        const Projection p = projectionOfFace(*f, info);
        if (!first) {
            first = f;
            firstBrush = b;
            firstProj = p;
            out.lightmap = lightmapScaleOf(*f);
            continue;
        }
        if (f->texture != first->texture) out.mixedTexture = true;
        if (std::abs(lightmapScaleOf(*f) - out.lightmap) > 1e-9) out.mixedLightmap = true;
        if (!near(p.scale, firstProj.scale) || !near(p.shift, firstProj.shift) || std::abs(p.rotate - firstProj.rotate) > 1e-9 || std::abs(p.shear - firstProj.shear) > 1e-9)
            out.mixedProjection = true;
    }
    if (!first) return out;
    out.valid = true;
    out.texture = first->texture;
    const SurfaceBasis basis = surfaceBasis(*first, info);
    out.texels = basis.texels;
    out.natural = basis.natural;
    out.shift = shiftTexels(firstProj, basis.texels);
    out.stretch = stretchOf(firstProj, basis.natural);
    out.rotate = firstProj.rotate;
    out.shear = firstProj.shear;
    out.kind = firstProj.kind;
    if (const nlohmann::json* raw = tp::faceKey(*first, "repeat")) {
        if (const auto rep = tp::parseFaceRepeat(*raw)) {
            if (rep->u) { out.repeatMode[0] = modeName(rep->u->mode); out.edge[0] = edgeName(rep->u->edge); }
            if (rep->v) { out.repeatMode[1] = modeName(rep->v->mode); out.edge[1] = edgeName(rep->v->edge); }
        }
    }
    if (info) {
        if (const tp::TexInfo* ti = info(first->texture.empty() ? "dev/grey" : first->texture))
            if (const auto heat = faceHeat(*firstBrush, *first, ti, info)) out.density = heatLabel(*heat);
    }
    return out;
}

} // namespace native::tools::tex
