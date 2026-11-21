// tools/texturing/flow -- see flow.h.
#include "tools/texturing/flow.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace native::tools::tex {

namespace {

using core::texplan::Aff;
using core::texplan::evalAff;
namespace tp = core::texplan;

struct Nb {
    uint32_t face;
    std::pair<uint32_t, uint32_t> edge;
};

V3 sub3(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double len3(const V3& a) { return std::sqrt(dot3(a, a)); }

// Faces sharing a full edge with each face (by vertex index pairs). The order of discovery is kept: it decides which of two equally short chains is taken.
std::map<uint32_t, std::vector<Nb>> neighbours(const core::Brush& b) {
    std::vector<std::pair<uint32_t, uint32_t>> keys;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> byEdge;
    for (const core::BrushFace& f : b.faces)
        for (size_t k = 0; k < f.loop.size(); ++k) {
            const uint32_t v = f.loop[k], w = f.loop[(k + 1) % f.loop.size()];
            const std::pair<uint32_t, uint32_t> key = v < w ? std::make_pair(v, w) : std::make_pair(w, v);
            auto it = byEdge.find(key);
            if (it == byEdge.end()) {
                keys.push_back(key);
                it = byEdge.emplace(key, std::vector<uint32_t>{}).first;
            }
            it->second.push_back(f.id);
        }
    std::map<uint32_t, std::vector<Nb>> out;
    for (const auto& key : keys) {
        const std::vector<uint32_t>& ids = byEdge[key];
        if (ids.size() != 2) continue;
        out[ids[0]].push_back({ids[1], key});
        out[ids[1]].push_back({ids[0], key});
    }
    return out;
}

// UVs of face B unfolded across the edge it shares with the solved face A (whose map is m).
std::vector<V2> unfold(const core::Brush& b, const core::BrushFace& A, const core::BrushFace& B, const std::pair<uint32_t, uint32_t>& edge, const tp::UvAffine& m) {
    const V3 p = tp::v3(b.vertices[edge.first]), q = tp::v3(b.vertices[edge.second]);
    const double el = len3(sub3(q, p));
    const V3 e{(q[0] - p[0]) / el, (q[1] - p[1]) / el, (q[2] - p[2]) / el};
    V3 c{0, 0, 0};
    for (uint32_t i : A.loop) {
        const V3 v = tp::v3(b.vertices[i]);
        const double k = 1.0 / static_cast<double>(A.loop.size());
        c = {c[0] + v[0] * k, c[1] + v[1] * k, c[2] + v[2] * k};
    }
    const V3 cp = sub3(c, p);
    const double along = dot3(cp, e);
    const V3 inward{cp[0] - e[0] * along, cp[1] - e[1] * along, cp[2] - e[2] * along};
    const double il = len3(inward);
    const V3 mA{inward[0] / il, inward[1] / il, inward[2] / il};
    std::vector<V2> out;
    for (uint32_t vi : B.loop) {
        const V3 x = tp::v3(b.vertices[vi]);
        const V3 xp = sub3(x, p);
        const double s = dot3(xp, e);
        const V3 w{xp[0] - e[0] * s, xp[1] - e[1] * s, xp[2] - e[2] * s};
        const double d = len3(w);
        const V3 y{p[0] + e[0] * s - mA[0] * d, p[1] + e[1] * s - mA[1] * d, p[2] + e[2] * s - mA[2] * d};
        out.push_back({evalAff(m.u, y), evalAff(m.v, y)});
    }
    return out;
}

} // namespace

std::optional<FlowResult> flowFaces(const core::Brush& b, uint32_t sourceId, const std::vector<uint32_t>& targetIds, const InfoFn& info) {
    const core::BrushFace* src = nullptr;
    for (const core::BrushFace& f : b.faces)
        if (f.id == sourceId) src = &f;
    if (!src || targetIds.empty()) return std::nullopt;
    const auto adj = neighbours(b);
    // breadth first from the source over the whole brush
    std::map<uint32_t, std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> parent;   // face -> (from, edge)
    std::vector<uint32_t> order{sourceId};
    std::set<uint32_t> seen{sourceId};
    for (size_t i = 0; i < order.size(); ++i) {
        const auto it = adj.find(order[i]);
        if (it == adj.end()) continue;
        for (const Nb& nb : it->second)
            if (seen.insert(nb.face).second) {
                parent[nb.face] = {order[i], nb.edge};
                order.push_back(nb.face);
            }
    }
    std::set<uint32_t> needed;
    for (uint32_t t : targetIds) {
        if (t == sourceId) continue;
        if (!seen.count(t)) return std::nullopt;
        for (uint32_t f = t; f != sourceId; f = parent[f].first) needed.insert(f);
    }
    std::map<uint32_t, tp::UvAffine> maps;
    maps[sourceId] = tp::faceAffine(b, *src, info);
    FlowResult out;
    out.brush = b;
    std::map<uint32_t, core::BrushFace> byId;
    for (uint32_t id : order) {
        if (!needed.count(id)) continue;
        const auto& par = parent[id];
        const auto fromIt = byId.find(par.first);
        const core::BrushFace& A = fromIt != byId.end() ? fromIt->second : *src;
        const core::BrushFace* B = nullptr;
        for (const core::BrushFace& f : b.faces)
            if (f.id == id) B = &f;
        const std::vector<V2> uvs = unfold(b, A, *B, par.second, maps[par.first]);
        const V3 n = tp::faceNormalOf(b, *B);
        std::vector<V3> pts;
        for (uint32_t i : B->loop) pts.push_back(tp::v3(b.vertices[i]));
        const auto m = tp::affineFromCorners(pts, uvs, n, 1e-6);
        if (!m) return std::nullopt;
        maps[id] = *m;
        const auto proj = projectionFromAffine(n, {m->u[0], m->u[1], m->u[2]}, {m->v[0], m->v[1], m->v[2]}, m->u[3], m->v[3]);
        core::BrushFace nf = *B;
        nf.texture = src->texture;
        if (proj) {
            nf = withProjectionUvs(nf, *proj, uvs);
        } else {
            nf.uvs.clear();
            for (const V2& uv : uvs) nf.uvs.push_back({uv[0], uv[1]});
        }
        // the repeat override travels with the texture
        nlohmann::json look = nf.look && nf.look->is_object() ? *nf.look : nlohmann::json::object();
        if (const nlohmann::json* rep = tp::faceKey(*src, "repeat")) look["repeat"] = *rep;
        else look.erase("repeat");
        if (look.empty()) nf.look.reset();
        else nf.look = std::make_shared<const nlohmann::json>(std::move(look));
        refreshLookHash(nf);
        byId[id] = nf;
        out.flowed.push_back(id);
    }
    for (core::BrushFace& f : out.brush.faces) {
        const auto it = byId.find(f.id);
        if (it != byId.end()) f = it->second;
    }
    return out;
}

} // namespace native::tools::tex
