// scene/relief -- see relief.h.
#include "scene/relief.h"

#include <map>
#include <unordered_map>

#include "scene/render_mesh.h"

namespace native::scene {

namespace tp = core::texplan;

namespace {

std::string faceKeyOf(const core::LightSurface& s) {
    return core::reliefKey(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.side);
}

std::vector<tp::V3> polygonOf(const core::LightSurface& s) {
    std::vector<tp::V3> poly;
    poly.reserve(s.verts.size());
    for (const core::Vec3& v : s.verts) poly.push_back(tp::v3(v));
    return poly;
}

// What the texture environment says about one authored face.
struct FaceTexturing {
    bool plain = true;                          // no repetition plan and no relief: the face's own uvs are all there is
    bool relief = false;                        // the face (or an alternate texture of its plan) shows a relief
    std::function<tp::V2(const tp::V3&)> uvAt;  // when plain
};

FaceTexturing texturingOf(const core::Brush& brush, const core::BrushFace& face, const ReliefEnv& env) {
    FaceTexturing ft;
    const std::string tex = face.texture.empty() ? std::string("dev/grey") : face.texture;
    const auto plan = tp::planForFace(brush, face, env.info, env.groups);
    if (plan) {
        for (const std::string& t : plan->textures)
            if (tp::reliefOf(env.info, t)) ft.relief = true;
    } else {
        ft.relief = static_cast<bool>(tp::reliefOf(env.info, tex));
    }
    ft.plain = !plan && !ft.relief;
    if (ft.plain) ft.uvAt = tp::faceUvAt(brush, face, env.info);
    return ft;
}

} // namespace

ObjectGeometry buildGeometry(const std::vector<core::LightSurface>& own, const std::vector<const std::vector<core::LightSurface>*>& around,
                             const ParentFaceFn& parent, const ReliefEnv& env, long long budgetTris) {
    ObjectGeometry out;
    auto relief = std::make_shared<ReliefList>();
    auto mesh = std::make_shared<RenderMesh>();
    out.relief = relief;
    out.mesh = mesh;
    if (own.empty()) return out;

    const auto vertex = [&](const tp::V3& p, const tp::V2& uv, uint32_t normal, uint32_t slot, uint16_t flags) {
        RenderVertex v{};
        v.pos[0] = static_cast<float>(p[0]);
        v.pos[1] = static_cast<float>(p[1]);
        v.pos[2] = static_cast<float>(p[2]);
        v.normal = normal;
        v.uv[0] = static_cast<float>(uv[0]);
        v.uv[1] = static_cast<float>(uv[1]);
        v.flags = flags;
        v.texture = slot;
        return v;
    };
    const auto octOf = [](const core::Vec3& n) { return octEncode(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)); };
    const auto octOfV3 = [](const tp::V3& n) { return octEncode(static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])); };
    const auto flagsOf = [](const std::string& texture) -> uint16_t { return isSkyTexture(texture) ? kFlagSky : uint16_t(0); };

    std::map<std::pair<const core::Brush*, uint32_t>, FaceTexturing> texturing;
    std::vector<std::pair<const core::Brush*, const core::BrushFace*>> parents(own.size());
    for (size_t i = 0; i < own.size(); ++i) {
        const core::LightSurface& s = own[i];
        parents[i] = env ? parent(s.brushInstance, s.brushId, s.faceId) : std::pair<const core::Brush*, const core::BrushFace*>{nullptr, nullptr};
        if (!parents[i].first || !parents[i].second) continue;
        const auto key = std::make_pair(parents[i].first, parents[i].second->id);
        if (texturing.find(key) == texturing.end()) texturing.emplace(key, texturingOf(*parents[i].first, *parents[i].second, env));
    }

    // outlines (made when the first polygon with a layout needs them): the polygons of the object first (index = position in `own`), then the neighbours'
    std::unique_ptr<tp::OutlineIndex> outlines;
    const auto outlinesOf = [&]() -> tp::OutlineIndex& {
        if (!outlines) {
            outlines = std::make_unique<tp::OutlineIndex>();
            for (const core::LightSurface& s : own) outlines->add(faceKeyOf(s), polygonOf(s), s.texture);
            for (const auto* list : around)
                if (list)
                    for (const core::LightSurface& s : *list) outlines->add(faceKeyOf(s), polygonOf(s), s.texture);
        }
        return *outlines;
    };

    tp::ReliefBudget budget;
    budget.left = budgetTris;
    std::map<std::string, core::ReliefMesh> meshes;   // by key: sorted, so the result does not depend on the order of the pieces
    for (size_t i = 0; i < own.size(); ++i) {
        const core::LightSurface& s = own[i];
        if (s.verts.size() < 3) continue;
        const auto pf = parents[i];
        const FaceTexturing* ft = nullptr;
        if (pf.first && pf.second) ft = &texturing.at(std::make_pair(pf.first, pf.second->id));

        if (!ft || ft->plain) {
            // the face as authored: fan triangles with the face's own uv map (or its projection)
            const uint32_t slot = mesh->slotOf(s.texture.empty() ? std::string("dev/grey") : s.texture);
            const uint32_t n = octOf(s.normal);
            const uint16_t flags = flagsOf(mesh->textures[slot]);
            const auto at = [&](const core::Vec3& p) {
                tp::V2 uv;
                if (ft) uv = ft->uvAt(tp::v3(p));
                else {
                    const core::Vec2 a = core::albedoUvAt(s, p);
                    uv = {a.u, a.v};
                }
                return vertex(tp::v3(p), uv, n, slot, flags);
            };
            for (size_t k = 1; k + 1 < s.verts.size(); ++k) mesh->addTriangle(at(s.verts[0]), at(s.verts[k]), at(s.verts[k + 1]));
            continue;
        }

        tp::OutlineIndex& oi = outlinesOf();
        const auto tris = tp::texturePolygon(polygonOf(s), tp::v3(s.normal), pf.first, pf.second, env.info, env.groups, &budget, "dev/grey", &oi.of(i));
        for (const tp::TexturedTri& t : tris) {
            const uint32_t slot = mesh->slotOf(t.texture.empty() ? std::string("dev/grey") : t.texture);
            const uint16_t flags = flagsOf(mesh->textures[slot]);
            mesh->addTriangle(vertex(t.pos[0], t.uv[0], octOfV3(t.normal[0]), slot, flags), vertex(t.pos[1], t.uv[1], octOfV3(t.normal[1]), slot, flags),
                              vertex(t.pos[2], t.uv[2], octOfV3(t.normal[2]), slot, flags));
        }
        if (!ft->relief) continue;
        const std::string key = faceKeyOf(s);
        auto it = meshes.find(key);
        if (it == meshes.end()) {
            core::ReliefMesh m;
            m.instance = s.instance;
            m.entityId = s.entityId;
            m.brushInstance = s.brushInstance;
            m.brushId = s.brushId;
            m.faceId = s.faceId;
            m.side = s.side;
            m.normal = s.normal;
            m.offset = s.normal.dot(s.verts[0]);
            it = meshes.emplace(key, std::move(m)).first;
        }
        for (const auto& t : tris)
            for (const auto& p : t.pos)
                for (double x : p) it->second.tris.push_back(static_cast<float>(x));
    }
    for (auto& [key, m] : meshes) {
        if (m.tris.size() < 9) continue;
        relief->push_back(core::ReliefSet::make(std::move(m)));
    }
    return out;
}

std::shared_ptr<const ReliefList> buildRelief(const std::vector<core::LightSurface>& own, const std::vector<const std::vector<core::LightSurface>*>& around,
                                              const ParentFaceFn& parent, const ReliefEnv& env, long long budgetTris) {
    return buildGeometry(own, around, parent, env, budgetTris).relief;
}

size_t reliefBytes(const ReliefList& list) {
    size_t n = sizeof(list) + list.capacity() * sizeof(void*);
    for (const auto& m : list) n += sizeof(core::ReliefMesh) + m->tris.capacity() * sizeof(float) + m->tris.size() * 6;   // + the BVH, roughly 24 bytes a triangle
    return n;
}

} // namespace native::scene
