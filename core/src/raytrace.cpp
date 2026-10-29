#include "core/raytrace.h"
#include "core/relief_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace core {

Vec3 OccluderMesh::boundsMin() const {
    Vec3 m{1e300, 1e300, 1e300};
    for (size_t i = 0; i + 2 < tris.size(); i += 3) m = {std::min<double>(m.x, tris[i]), std::min<double>(m.y, tris[i + 1]), std::min<double>(m.z, tris[i + 2])};
    return tris.empty() ? Vec3{} : m;
}
Vec3 OccluderMesh::boundsMax() const {
    Vec3 m{-1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < tris.size(); i += 3) m = {std::max<double>(m.x, tris[i]), std::max<double>(m.y, tris[i + 1]), std::max<double>(m.z, tris[i + 2])};
    return tris.empty() ? Vec3{} : m;
}

OccluderMesh buildOccluders(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightmapLayout* layout) {
    OccluderMesh m;
    std::unordered_map<std::string, int32_t> textureIds;
    std::unordered_set<const ReliefMesh*> reliefDone;
    for (const auto& s : surfaces) {
        if (!classes.castsShadows(s.classname) || !s.castsShadow || s.verts.size() < 3) continue;
        // A face with a relief is its relief: the flat pieces are left out (a pit would be covered by them) and the relief's triangles go in once per face
        // (all its pieces share the mesh). Lightmap and albedo coordinates of those come from each corner's footprint on the flat plane.
        const ReliefMesh* relief = s.relief.get();
        if (relief && !reliefDone.insert(relief).second) continue;
        const LightmapFace* face = layout ? layout->findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts) : nullptr;
        const int page = face && face->chart >= 0 ? layout->charts[static_cast<size_t>(face->chart)].page : 0;
        auto addTriangle = [&](const Vec3 (&tri)[3], const Vec3 (&at)[3]) {
            for (const Vec3& v : tri) {
                m.tris.push_back(static_cast<float>(v.x));
                m.tris.push_back(static_cast<float>(v.y));
                m.tris.push_back(static_cast<float>(v.z));
            }
            if (!layout) return;
            int32_t tex = -1;
            if (!s.texture.empty()) {
                const auto ins = textureIds.emplace(s.texture, static_cast<int32_t>(m.textureNames.size()));
                if (ins.second) m.textureNames.push_back(s.texture);
                tex = ins.first->second;
            }
            m.triTexture.push_back(tex);
            Vec2 uv[3], lm[3];
            for (int c = 0; c < 3; ++c) {
                uv[c] = albedoUvAt(s, at[c]);  // the face's own UV map when it has one (REQUIREMENTS §24 C3)
                lm[c] = face ? face->texel(at[c]) : Vec2{0, 0};
                m.albedoUv.push_back(static_cast<float>(uv[c].u));
                m.albedoUv.push_back(static_cast<float>(uv[c].v));
            }
            const double aUv = 0.5 * std::fabs((uv[1].u - uv[0].u) * (uv[2].v - uv[0].v) - (uv[1].v - uv[0].v) * (uv[2].u - uv[0].u));
            const double aLm = 0.5 * std::fabs((lm[1].u - lm[0].u) * (lm[2].v - lm[0].v) - (lm[1].v - lm[0].v) * (lm[2].u - lm[0].u));
            m.uvRatio.push_back(aLm > 1e-9 ? static_cast<float>(aUv / aLm) : 0.f);
            // pack as 2 x vec4: (u0 v0 u1 v1) (u2 v2 page valid)
            m.lightmap.insert(m.lightmap.end(), {static_cast<float>(lm[0].u), static_cast<float>(lm[0].v), static_cast<float>(lm[1].u), static_cast<float>(lm[1].v),
                                                 static_cast<float>(lm[2].u), static_cast<float>(lm[2].v), static_cast<float>(page), face ? 1.f : 0.f});
        };
        if (relief) {
            for (size_t i = 0; i + 8 < relief->tris.size(); i += 9) {
                const float* t = &relief->tris[i];
                const Vec3 tri[3] = {{t[0], t[1], t[2]}, {t[3], t[4], t[5]}, {t[6], t[7], t[8]}};
                const Vec3 at[3] = {relief->footprint(tri[0]), relief->footprint(tri[1]), relief->footprint(tri[2])};
                addTriangle(tri, at);
            }
            continue;
        }
        for (size_t k = 1; k + 1 < s.verts.size(); ++k) {
            const Vec3 tri[3] = {s.verts[0], s.verts[k], s.verts[k + 1]};
            addTriangle(tri, tri);
        }
    }
    return m;
}

void attachAlbedo(OccluderMesh& mesh, const AlbedoLoader& loader) {
    mesh.albedo.reset();
    mesh.albedoMeta.clear();
    const size_t tris = mesh.triangleCount();
    if (!loader || mesh.triTexture.size() != tris || mesh.albedoUv.size() != tris * 6 || mesh.uvRatio.size() != tris || mesh.textureNames.empty()) return;
    // compact ids: only textures that loaded get a slot (the GPU array has no holes)
    auto set = std::make_shared<AlbedoSet>();
    std::vector<int32_t> remap(mesh.textureNames.size(), -1);
    for (size_t i = 0; i < mesh.textureNames.size(); ++i) {
        auto img = loader(mesh.textureNames[i]);
        if (!img || img->mips.empty()) continue;
        remap[i] = static_cast<int32_t>(set->images.size());
        set->names.push_back(mesh.textureNames[i]);
        set->images.push_back(std::move(img));
    }
    if (set->images.empty()) return;
    mesh.albedoMeta.resize(tris * 8);
    for (size_t t = 0; t < tris; ++t) {
        float* m = &mesh.albedoMeta[t * 8];
        for (int k = 0; k < 6; ++k) m[k] = mesh.albedoUv[t * 6 + static_cast<size_t>(k)];
        const int32_t id = mesh.triTexture[t] >= 0 ? remap[static_cast<size_t>(mesh.triTexture[t])] : -1;
        float lod = 0.f;
        if (id >= 0) {
            const AlbedoImage& im = *set->images[static_cast<size_t>(id)];
            // texels of the texture per lightmap texel (area), as a linear footprint -> level of detail
            const double texels = static_cast<double>(mesh.uvRatio[t]) * im.width * im.height;
            lod = texels > 1.0 ? static_cast<float>(0.5 * std::log2(texels)) : 0.f;
            lod = std::min(lod, static_cast<float>(im.mipCount() - 1));
        }
        m[6] = static_cast<float>(id);
        m[7] = lod;
    }
    // transparency (REQUIREMENTS §23): headers for every id, texels of the non-opaque textures
    mesh.alphaWords.clear();
    mesh.transparentTriangles = 0;
    const size_t ids = set->images.size();
    std::vector<uint32_t> words(ids * 8, 0u);
    bool any = false;
    for (size_t id = 0; id < ids; ++id) {
        const AlbedoImage& im = *set->images[id];
        const Transparency& tr = im.transparency;
        if (tr.mode == TransparencyMode::Opaque || im.mips.empty()) continue;
        any = true;
        const int gw = im.hasAlpha() ? im.alphaWidth : im.width, gh = im.hasAlpha() ? im.alphaHeight : im.height;
        uint32_t* h = &words[id * 8];
        h[0] = static_cast<uint32_t>(words.size());
        h[1] = static_cast<uint32_t>(gw);
        h[2] = static_cast<uint32_t>(gh);
        h[3] = static_cast<uint32_t>(tr.mode);
        std::memcpy(&h[4], &tr.cutoff, 4);
        std::memcpy(&h[5], &tr.opacity, 4);
        std::memcpy(&h[6], &tr.tint, 4);
        const auto& m0 = im.mips[0];
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int cx = std::min(im.width - 1, static_cast<int>((x + 0.5) * im.width / gw)), cy = std::min(im.height - 1, static_cast<int>((y + 0.5) * im.height / gh));
                const size_t o = (static_cast<size_t>(cy) * static_cast<size_t>(im.width) + static_cast<size_t>(cx)) * 4;
                const uint32_t a = im.hasAlpha() ? im.alpha[static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)] : 255u;
                words.push_back(static_cast<uint32_t>(m0[o]) | (static_cast<uint32_t>(m0[o + 1]) << 8) | (static_cast<uint32_t>(m0[o + 2]) << 16) | (a << 24));
            }
    }
    if (any) {
        for (size_t t = 0; t < tris; ++t) {
            const int id = static_cast<int>(std::floor(mesh.albedoMeta[t * 8 + 6] + 0.5f));
            if (id >= 0 && words[static_cast<size_t>(id) * 8 + 3] != 0u) ++mesh.transparentTriangles;
        }
        if (mesh.transparentTriangles) mesh.alphaWords = std::move(words);
    }
    mesh.albedo = std::move(set);
}

int surfaceAt(const float* rec, const std::vector<uint32_t>& words, double b1, double b2, Vec3& transmit) {
    const int id = static_cast<int>(std::floor(rec[6] + 0.5f));
    if (id < 0 || static_cast<size_t>(id) * 8 + 7 >= words.size()) return 0;
    const uint32_t* h = &words[static_cast<size_t>(id) * 8];
    if (h[3] == 0u) return 0;
    const double w0 = 1.0 - b1 - b2;
    const double u = w0 * rec[0] + b1 * rec[2] + b2 * rec[4], v = w0 * rec[1] + b1 * rec[3] + b2 * rec[5];
    const int w = static_cast<int>(h[1]), hh = static_cast<int>(h[2]);
    // the shader's formula exactly (float precision, fract, clamp): both sides pick the same texel
    auto wrap = [](double x, int n) { const float f = static_cast<float>(x) - std::floor(static_cast<float>(x)); return static_cast<size_t>(std::min(n - 1, static_cast<int>(f * static_cast<float>(n)))); };
    const uint32_t px = words[h[0] + wrap(v, hh) * static_cast<size_t>(w) + wrap(u, w)];
    const float a = static_cast<float>(px >> 24) / 255.f;
    float cutoff, opacity, tint;
    std::memcpy(&cutoff, &h[4], 4);
    std::memcpy(&opacity, &h[5], 4);
    std::memcpy(&tint, &h[6], 4);
    if (h[3] == 1u) return a < cutoff ? -1 : 0;
    if ((px >> 24) < 16u) return -1;  // translucent: a clear texel (classifyAlpha's "clear") is no surface, whatever rgb is stored under it
    const Vec3 colour{byteToUnit(static_cast<uint8_t>(px & 255u)), byteToUnit(static_cast<uint8_t>((px >> 8) & 255u)), byteToUnit(static_cast<uint8_t>((px >> 16) & 255u))};
    const double k = 1.0 - static_cast<double>(opacity) * a;
    transmit = Vec3{1 + (colour.x - 1) * tint, 1 + (colour.y - 1) * tint, 1 + (colour.z - 1) * tint} * k;
    return 1;
}

namespace {
constexpr uint32_t kLeafSize = 4;

// Moller-Trumbore, double sided. Returns t or -1; `bu` / `bv` (optional) get the weights of vertices 1 and 2.
inline double hitTriangle(const float* t, const Vec3& o, const Vec3& d, double tmax, double* bu = nullptr, double* bv = nullptr) {
    const double e1x = t[3] - t[0], e1y = t[4] - t[1], e1z = t[5] - t[2];
    const double e2x = t[6] - t[0], e2y = t[7] - t[1], e2z = t[8] - t[2];
    const double px = d.y * e2z - d.z * e2y, py = d.z * e2x - d.x * e2z, pz = d.x * e2y - d.y * e2x;
    const double det = e1x * px + e1y * py + e1z * pz;
    if (std::fabs(det) < 1e-14) return -1;
    const double inv = 1.0 / det;
    const double tx = o.x - t[0], ty = o.y - t[1], tz = o.z - t[2];
    const double u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0 || u > 1) return -1;
    const double qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
    const double v = (d.x * qx + d.y * qy + d.z * qz) * inv;
    if (v < 0 || u + v > 1) return -1;
    const double dist = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (bu) *bu = u;
    if (bv) *bv = v;
    return dist >= 0 && dist <= tmax ? dist : -1;
}

inline bool hitBox(const float* mn, const float* mx, const Vec3& o, const Vec3& invD, double tmax) {
    double t0 = 0, t1 = tmax;
    const double org[3] = {o.x, o.y, o.z}, inv[3] = {invD.x, invD.y, invD.z};
    for (int a = 0; a < 3; ++a) {
        double ta = (mn[a] - org[a]) * inv[a], tb = (mx[a] - org[a]) * inv[a];
        if (std::isnan(ta)) ta = -std::numeric_limits<double>::infinity();
        if (std::isnan(tb)) tb = std::numeric_limits<double>::infinity();
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}
}  // namespace

TriangleBvh::TriangleBvh(const OccluderMesh& mesh) {
    const uint32_t n = static_cast<uint32_t>(mesh.triangleCount());
    if (!n) return;
    std::vector<uint32_t> order(n);
    std::vector<std::array<float, 3>> centroids(n);
    for (uint32_t i = 0; i < n; ++i) {
        order[i] = i;
        const float* t = &mesh.tris[i * 9];
        centroids[i] = {(t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3, (t[2] + t[5] + t[8]) / 3};
    }
    nodes_.reserve(2 * n / kLeafSize + 2);
    nodes_.push_back({});
    build(0, order, centroids, 0, n, mesh.tris);
    tris_.resize(mesh.tris.size());
    for (uint32_t i = 0; i < n; ++i) std::copy_n(&mesh.tris[order[i] * 9], 9, &tris_[i * 9]);
    order_ = std::move(order);
    if (mesh.hasTransparency()) { albedoMeta_ = mesh.albedoMeta; alphaWords_ = mesh.alphaWords; }
}

int TriangleBvh::surface(uint32_t slot, double b1, double b2, Vec3& transmit) const {
    if (alphaWords_.empty()) return 0;
    return surfaceAt(&albedoMeta_[static_cast<size_t>(order_[slot]) * 8], alphaWords_, b1, b2, transmit);
}

void TriangleBvh::build(uint32_t node, std::vector<uint32_t>& order, std::vector<std::array<float, 3>>& centroids, uint32_t begin, uint32_t end,
                        const std::vector<float>& src) {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    float cmn[3] = {1e30f, 1e30f, 1e30f}, cmx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = begin; i < end; ++i) {
        const float* t = &src[order[i] * 9];
        for (int v = 0; v < 3; ++v)
            for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], t[v * 3 + a]); mx[a] = std::max(mx[a], t[v * 3 + a]); }
        for (int a = 0; a < 3; ++a) { cmn[a] = std::min(cmn[a], centroids[order[i]][a]); cmx[a] = std::max(cmx[a], centroids[order[i]][a]); }
    }
    std::copy(mn, mn + 3, nodes_[node].mn);
    std::copy(mx, mx + 3, nodes_[node].mx);
    int axis = 0;
    for (int a = 1; a < 3; ++a) if (cmx[a] - cmn[a] > cmx[axis] - cmn[axis]) axis = a;
    if (end - begin <= kLeafSize || cmx[axis] - cmn[axis] < 1e-9f) {
        nodes_[node].left = begin;
        nodes_[node].count = end - begin;
        return;
    }
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                     [&](uint32_t a, uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });
    const uint32_t left = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({});
    nodes_.push_back({});
    nodes_[node].left = left;
    nodes_[node].count = 0;
    build(left, order, centroids, begin, mid, src);
    build(left + 1, order, centroids, mid, end, src);
}

TriangleBvh::Hit TriangleBvh::closestHit(const Vec3& o, const Vec3& d, double tmax) const {
    Hit best;
    if (nodes_.empty()) return best;
    const Vec3 invD{d.x != 0 ? 1.0 / d.x : std::numeric_limits<double>::infinity(), d.y != 0 ? 1.0 / d.y : std::numeric_limits<double>::infinity(),
                    d.z != 0 ? 1.0 / d.z : std::numeric_limits<double>::infinity()};
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!hitBox(nd.mn, nd.mx, o, invD, best.t >= 0 ? best.t : tmax)) continue;
        if (nd.count) {
            for (uint32_t i = 0; i < nd.count; ++i) {
                const float* tr = &tris_[(nd.left + i) * 9];
                double bu = 0, bv = 0;
                const double t = hitTriangle(tr, o, d, best.t >= 0 ? best.t : tmax, &bu, &bv);
                if (t < 0) continue;
                Vec3 tm;
                if (surface(nd.left + i, bu, bv, tm) < 0) continue;  // a hole of a cutout texture
                best.t = t;
                best.triangle = order_[nd.left + i];
                // barycentrics of the hit point
                const Vec3 p = o + d * t, a{tr[0], tr[1], tr[2]}, e1 = Vec3{tr[3], tr[4], tr[5]} - a, e2 = Vec3{tr[6], tr[7], tr[8]} - a, w = p - a;
                const double d11 = e1.dot(e1), d12 = e1.dot(e2), d22 = e2.dot(e2), dw1 = w.dot(e1), dw2 = w.dot(e2);
                const double den = d11 * d22 - d12 * d12;
                if (std::fabs(den) > 1e-30) {
                    best.b1 = (d22 * dw1 - d12 * dw2) / den;
                    best.b2 = (d11 * dw2 - d12 * dw1) / den;
                }
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.left + 1;
        }
    }
    return best;
}

double TriangleBvh::nearestHit(const Vec3& o, const Vec3& d, double tmax) const {
    if (nodes_.empty()) return -1;
    const Vec3 invD{d.x != 0 ? 1.0 / d.x : std::numeric_limits<double>::infinity(), d.y != 0 ? 1.0 / d.y : std::numeric_limits<double>::infinity(),
                    d.z != 0 ? 1.0 / d.z : std::numeric_limits<double>::infinity()};
    double best = -1;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!hitBox(nd.mn, nd.mx, o, invD, best >= 0 ? best : tmax)) continue;
        if (nd.count) {
            for (uint32_t i = 0; i < nd.count; ++i) {
                double bu = 0, bv = 0;
                const double t = hitTriangle(&tris_[(nd.left + i) * 9], o, d, best >= 0 ? best : tmax, &bu, &bv);
                Vec3 tm;
                if (t >= 0 && surface(nd.left + i, bu, bv, tm) >= 0) best = t;
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.left + 1;
        }
    }
    return best;
}

Vec3 TriangleBvh::transmittance(const Vec3& o, const Vec3& d, double tmax) const {
    Vec3 T{1, 1, 1};
    if (nodes_.empty()) return T;
    const Vec3 invD{d.x != 0 ? 1.0 / d.x : std::numeric_limits<double>::infinity(), d.y != 0 ? 1.0 / d.y : std::numeric_limits<double>::infinity(),
                    d.z != 0 ? 1.0 / d.z : std::numeric_limits<double>::infinity()};
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!hitBox(nd.mn, nd.mx, o, invD, tmax)) continue;
        if (nd.count) {
            for (uint32_t i = 0; i < nd.count; ++i) {
                double bu = 0, bv = 0;
                if (hitTriangle(&tris_[(nd.left + i) * 9], o, d, tmax, &bu, &bv) < 0) continue;
                Vec3 tm;
                const int s = surface(nd.left + i, bu, bv, tm);
                if (s < 0) continue;
                if (s == 0) return {0, 0, 0};
                // a translucent pane is a closed brush: the light is dimmed once, where it enters (front side), not again where it leaves
                const float* tr = &tris_[(nd.left + i) * 9];
                const Vec3 a{tr[0], tr[1], tr[2]}, nrm = (Vec3{tr[3], tr[4], tr[5]} - a).cross(Vec3{tr[6], tr[7], tr[8]} - a);
                if (nrm.dot(d) >= 0) continue;
                T = Vec3{T.x * tm.x, T.y * tm.y, T.z * tm.z};
                if (std::max({T.x, T.y, T.z}) < 1e-3) return {0, 0, 0};
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.left + 1;
        }
    }
    return T;
}

bool TriangleBvh::occluded(const Vec3& o, const Vec3& d, double tmax) const {
    if (nodes_.empty()) return false;
    if (!alphaWords_.empty()) { const Vec3 T = transmittance(o, d, tmax); return std::max({T.x, T.y, T.z}) < 1e-3; }
    const Vec3 invD{d.x != 0 ? 1.0 / d.x : std::numeric_limits<double>::infinity(), d.y != 0 ? 1.0 / d.y : std::numeric_limits<double>::infinity(),
                    d.z != 0 ? 1.0 / d.z : std::numeric_limits<double>::infinity()};
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!hitBox(nd.mn, nd.mx, o, invD, tmax)) continue;
        if (nd.count) {
            for (uint32_t i = 0; i < nd.count; ++i)
                if (hitTriangle(&tris_[(nd.left + i) * 9], o, d, tmax) >= 0) return true;
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.left + 1;
        }
    }
    return false;
}

void TriangleBvh::exportGpu(std::vector<float>& nodes, std::vector<float>& triangles) const {
    nodes.resize(nodes_.size() * 8);
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        float* o = &nodes[i * 8];
        o[0] = n.mn[0]; o[1] = n.mn[1]; o[2] = n.mn[2];
        std::memcpy(&o[3], &n.left, sizeof(uint32_t));
        o[4] = n.mx[0]; o[5] = n.mx[1]; o[6] = n.mx[2];
        std::memcpy(&o[7], &n.count, sizeof(uint32_t));
    }
    triangles = tris_;
}

}  // namespace core
