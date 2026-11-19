// scene/relief -- see relief.h.
#include "scene/relief.h"

#include <map>
#include <unordered_map>

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

// Does the face show a relief: its texture's, or an alternate texture's of its repetition plan.
bool faceHasRelief(const core::Brush& brush, const core::BrushFace& face, const ReliefEnv& env) {
    const auto plan = tp::planForFace(brush, face, env.info, env.groups);
    if (plan) {
        for (const std::string& t : plan->textures)
            if (tp::reliefOf(env.info, t)) return true;
        return false;
    }
    return static_cast<bool>(tp::reliefOf(env.info, face.texture.empty() ? std::string("dev/grey") : face.texture));
}

} // namespace

std::shared_ptr<const ReliefList> buildRelief(const std::vector<core::LightSurface>& own, const std::vector<const std::vector<core::LightSurface>*>& around,
                                              const ParentFaceFn& parent, const ReliefEnv& env, long long budgetTris) {
    auto out = std::make_shared<ReliefList>();
    if (!env || own.empty()) return out;

    // which pieces' faces show a relief (once per authored face)
    std::map<std::pair<const core::Brush*, uint32_t>, bool> hasRelief;
    std::vector<std::pair<const core::Brush*, const core::BrushFace*>> parents(own.size());
    bool any = false;
    for (size_t i = 0; i < own.size(); ++i) {
        const core::LightSurface& s = own[i];
        parents[i] = parent(s.brushInstance, s.brushId, s.faceId);
        if (!parents[i].first || !parents[i].second) continue;
        const auto key = std::make_pair(parents[i].first, parents[i].second->id);
        auto it = hasRelief.find(key);
        if (it == hasRelief.end()) it = hasRelief.emplace(key, faceHasRelief(*parents[i].first, *parents[i].second, env)).first;
        any = any || it->second;
    }
    if (!any) return out;

    // outlines: the polygons of the object first (their index = their position in `own`), then the neighbours'
    tp::OutlineIndex outlines;
    for (const core::LightSurface& s : own) outlines.add(faceKeyOf(s), polygonOf(s), s.texture);
    for (const auto* list : around)
        if (list)
            for (const core::LightSurface& s : *list) outlines.add(faceKeyOf(s), polygonOf(s), s.texture);

    tp::ReliefBudget budget;
    budget.left = budgetTris;
    std::map<std::string, core::ReliefMesh> meshes;   // by key: sorted, so the result does not depend on the order of the pieces
    for (size_t i = 0; i < own.size(); ++i) {
        const core::LightSurface& s = own[i];
        if (!parents[i].first || !parents[i].second) continue;
        if (!hasRelief[std::make_pair(parents[i].first, parents[i].second->id)]) continue;
        const auto tris = tp::texturePolygon(polygonOf(s), tp::v3(s.normal), parents[i].first, parents[i].second, env.info, env.groups, &budget, "dev/grey",
                                             &outlines.of(i));
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
        out->push_back(core::ReliefSet::make(std::move(m)));
    }
    return out;
}

size_t reliefBytes(const ReliefList& list) {
    size_t n = sizeof(list) + list.capacity() * sizeof(void*);
    for (const auto& m : list) n += sizeof(core::ReliefMesh) + m->tris.capacity() * sizeof(float) + m->tris.size() * 6;   // + the BVH, roughly 24 bytes a triangle
    return n;
}

} // namespace native::scene
