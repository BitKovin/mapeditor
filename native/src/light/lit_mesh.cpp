// light/lit_mesh -- see lit_mesh.h.
#include "light/lit_mesh.h"

#include <algorithm>

namespace native::light {

std::shared_ptr<const scene::RenderMesh> makeLitMesh(const scene::RenderMesh& source, const std::vector<core::LightSurface>& surfaces, const core::LightmapLayout& layout,
                                                     LitMeshStats* stats) {
    if (source.pieces.size() * 3 != source.indices.size()) return nullptr;   // a raw mesh: no pieces
    // the chart of every piece, found once
    struct Found {
        const core::LightmapFace* face = nullptr;
        uint16_t page = 0;
        bool done = false;
    };
    std::vector<Found> found(surfaces.size());
    const auto lookup = [&](uint32_t piece) -> const Found& {
        Found& f = found[piece];
        if (f.done) return f;
        f.done = true;
        const core::LightSurface& s = surfaces[piece];
        if (s.verts.size() < 3) return f;
        const core::LightmapFace* face = layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (face && face->chart >= 0 && static_cast<size_t>(face->chart) < layout.charts.size()) {
            f.face = face;
            f.page = static_cast<uint16_t>(layout.charts[static_cast<size_t>(face->chart)].page);
        }
        return f;
    };

    auto out = std::make_shared<scene::RenderMesh>(source);
    LitMeshStats local;
    for (size_t t = 0; t < source.pieces.size(); ++t) {
        const uint32_t piece = source.pieces[t];
        const Found* f = piece < surfaces.size() ? &lookup(piece) : nullptr;
        if (!f || !f->face) {
            ++local.unlitTriangles;
            continue;
        }
        ++local.litTriangles;
        for (int k = 0; k < 3; ++k) {
            scene::RenderVertex& v = out->vertices[source.indices[t * 3 + static_cast<size_t>(k)]];
            const core::Vec2 uv = layout.uv(*f->face, core::Vec3{v.pos[0], v.pos[1], v.pos[2]});
            v.lm[0] = static_cast<float>(uv.u);
            v.lm[1] = static_cast<float>(uv.v);
            v.page = f->page;
            v.flags = static_cast<uint16_t>(v.flags | scene::kFlagLit);
        }
    }
    if (stats) *stats = local;
    if (local.litTriangles == 0) return nullptr;
    return out;
}

} // namespace native::light
